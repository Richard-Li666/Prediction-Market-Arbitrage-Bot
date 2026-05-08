#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <csignal>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <limits>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/clock.hpp"
#include "logging/jsonl_writer.hpp"
#include "polymarket/bucket_market_discovery.hpp"
#include "polymarket/polymarket_web_ptb.hpp"
#include "polymarket/rtds_chainlink_feed.hpp"
#include "polymarket/rtds_chainlink_strike.hpp"
#include "polymarket/ws_fixed_token_feed.hpp"
#include "telemetry/pipeline.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_sig(int) { g_stop = true; }

// --- Hot-path state cache (Scheme A): keep latest Binance + Poly quotes in memory and
// compute simple theoretical probabilities without disk roundtrips.
struct HotState {
  std::mutex mu;

  // Market alignment: follow Polymarket discovery/rollover.
  std::int64_t active_epoch{-1};  // seconds since epoch, 5m bucket start
  std::string slug;

  // Binance latest
  bool have_bin{false};
  double bin_mid{0.0};
  std::int64_t bin_wall_ms{0};

  // Rolling buffer for realized vol (wall_ms, mid). Keep ~1h.
  std::deque<std::pair<std::int64_t, double>> bin_hist;

      // Strike from Gamma eventMetadata.priceToBeat (initialized after discovery).
      bool have_K{false};
      double K{0.0};

  // Per-bucket sigma snapshot (computed from 1h realized at bucket start).
  double sigma_bucket{0.0};
  bool have_sigma{false};

  // Poly latest quotes (current slug)
  bool have_up{false};
  double up_bid{0.0};
  double up_ask{0.0};
  std::int64_t up_wall_ms{0};

  bool have_down{false};
  double down_bid{0.0};
  double down_ask{0.0};
  std::int64_t down_wall_ms{0};

  // Rate-limit prints
  std::int64_t last_print_mono_ns{0};
};

static inline double normal_cdf(double x) {
  // N(x) = 0.5 * erfc(-x / sqrt(2))
  return 0.5 * std::erfc(-x * M_SQRT1_2);
}

static inline double d2(double S, double K, double T_years, double sigma) {
  if (sigma <= 0 || T_years <= 0 || S <= 0 || K <= 0) {
    return (S > K) ? 1e9 : -1e9;
  }
  const double v = sigma * std::sqrt(T_years);
  const double m = std::log(S / K) - 0.5 * sigma * sigma * T_years;
  return m / v;
}

static inline double digital_call_prob(double S, double K, double T_years, double sigma) {
  return normal_cdf(d2(S, K, T_years, sigma));
}

static inline double realized_vol_1h_resampled_from_hist(
    const std::deque<std::pair<std::int64_t, double>>& hist,
    std::int64_t t_end_wall_ms,
    std::int64_t step_ms) {
  // Matches notebook logic: resample with forward fill to fixed grid, then annualize var(log-returns).
  const std::int64_t t0 = t_end_wall_ms - 3600'000;
  if (step_ms <= 0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  // Find starting index in hist (hist is time-ordered).
  std::size_t start = 0;
  while (start < hist.size() && hist[start].first < t0) {
    ++start;
  }
  if (start >= hist.size()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  // Grid length ~ 3600s / step.
  const std::size_t n = static_cast<std::size_t>((3600'000 / step_ms) + 1);
  if (n < 3) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  std::vector<double> mids;
  mids.reserve(n);

  std::size_t j = start;
  double last = hist[j].second;
  for (std::int64_t tg = t0; tg <= t_end_wall_ms; tg += step_ms) {
    while (j < hist.size() && hist[j].first <= tg) {
      last = hist[j].second;
      ++j;
    }
    mids.push_back(last);
  }
  if (mids.size() < 3) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // log returns
  std::vector<double> rets;
  rets.reserve(mids.size() - 1);
  for (std::size_t i = 1; i < mids.size(); ++i) {
    if (mids[i - 1] > 0 && mids[i] > 0) {
      rets.push_back(std::log(mids[i] / mids[i - 1]));
    }
  }
  if (rets.size() < 2) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  // sample variance
  double mean = 0.0;
  for (double x : rets) {
    mean += x;
  }
  mean /= static_cast<double>(rets.size());
  double var = 0.0;
  for (double x : rets) {
    const double d = x - mean;
    var += d * d;
  }
  var /= static_cast<double>(rets.size() - 1);

  const double steps_per_year =
      (365.0 * 24.0 * 3600.0 * 1000.0) / static_cast<double>(step_ms);
  return std::sqrt(var * steps_per_year);
}

class AsyncJsonlWriter {
 public:
  explicit AsyncJsonlWriter(const std::string& path, std::size_t max_queue = 10000)
      : writer_(path), max_queue_(max_queue) {
    thr_ = std::thread([this] { run(); });
  }

  ~AsyncJsonlWriter() { stop_and_flush(); }

  void enqueue(nlohmann::json row) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_not_full_.wait(lk, [this] { return stop_ || q_.size() < max_queue_; });
    if (stop_) {
      return;
    }
    q_.push(std::move(row));
    lk.unlock();
    cv_.notify_one();
  }

  void stop_and_flush() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (stop_) {
        return;
      }
      stop_ = true;
    }
    cv_.notify_all();
    cv_not_full_.notify_all();
    if (thr_.joinable()) {
      thr_.join();
    }
    writer_.flush();
  }

 private:
  void run() {
    for (;;) {
      nlohmann::json row;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
        if (stop_ && q_.empty()) {
          return;
        }
        row = std::move(q_.front());
        q_.pop();
      }
      cv_not_full_.notify_one();
      writer_.append(row);
    }
  }

  ll::logging::JsonlWriter writer_;
  const std::size_t max_queue_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable cv_not_full_;
  std::queue<nlohmann::json> q_;
  bool stop_{false};
  std::thread thr_;
};

bool parse_epoch_from_confirmed_slug(const std::string& slug, std::int64_t* out_epoch) {
  static constexpr char kPrefix[] = "btc-updown-5m-";
  constexpr std::size_t plen = sizeof(kPrefix) - 1;
  if (slug.size() <= plen) {
    return false;
  }
  for (std::size_t i = 0; i < plen; ++i) {
    if (std::tolower(static_cast<unsigned char>(slug[i])) !=
        static_cast<unsigned char>(kPrefix[i])) {
      return false;
    }
  }
  try {
    *out_epoch = std::stoll(slug.substr(plen));
    return true;
  } catch (...) {
    return false;
  }
}

constexpr std::int64_t kLivePipeRtdsTimeoutMs = 15000;
constexpr std::int64_t kLivePipeRtdsMaxSkewMs = 180000;

bool live_pipe_disc_has_strike(const ll::polymarket::BtcFiveMinuteBucketDiscovery& d) {
  return std::isfinite(d.price_to_beat) && d.price_to_beat > 0.0;
}

void live_pipe_maybe_rtds_strike(ll::polymarket::BtcFiveMinuteBucketDiscovery& d, const char* log_pfx) {
  if (d.gamma_has_price_to_beat && live_pipe_disc_has_strike(d)) {
    return;
  }
  std::string rerr;
  if (ll::polymarket::fill_strike_from_polymarket_rtds_chainlink(d, kLivePipeRtdsTimeoutMs,
                                                                 kLivePipeRtdsMaxSkewMs, &rerr)) {
    std::cerr << log_pfx << "[rtds_chainlink] strike=" << d.price_to_beat << " target_wall_ms="
              << d.event_start_wall_ms << "\n";
    return;
  }
  std::cerr << log_pfx << "[rtds_chainlink] strike unavailable: " << rerr << "\n";
}

void live_pipe_maybe_web_then_rtds_strike(ll::polymarket::BtcFiveMinuteBucketDiscovery& d, bool try_web_first,
                                          const char* log_pfx) {
  if (d.gamma_has_price_to_beat && live_pipe_disc_has_strike(d)) {
    return;
  }
  if (try_web_first) {
    std::string werr;
    if (ll::polymarket::fill_strike_from_polymarket_web_event_page(d, &werr)) {
      std::cerr << log_pfx << "[web_ptb] strike=" << d.price_to_beat << " slug=" << d.confirmed_slug << "\n";
      return;
    }
    std::cerr << log_pfx << "[web_ptb] unavailable: " << werr << "\n";
  }
  live_pipe_maybe_rtds_strike(d, log_pfx);
}

void append_poly_quote_row(AsyncJsonlWriter& writer, std::atomic<std::uint64_t>& seq,
                           const ll::polymarket::PolymarketWsQuote& q) {
  const auto n = seq.fetch_add(1, std::memory_order_relaxed) + 1;
  nlohmann::json row;
  row["schema_version"] = 1;
  row["source"] = "polymarket";
  row["event_type"] = "quote";
  row["event_slug"] = q.event_slug;
  row["outcome"] = q.outcome;
  if (q.market_bucket_epoch >= 0) {
    row["market_bucket_epoch"] = q.market_bucket_epoch;
  }
  row["symbol_or_market_id"] = q.token_id;
  row["local_ts_mono_ns"] = q.local_mono_ns;
  row["local_ts_wall_ms"] = q.local_wall_ms;
  row["seq"] = n;
  row["payload"] = {{"best_bid", q.best_bid}, {"best_ask", q.best_ask}};
  writer.enqueue(std::move(row));
}

void append_poly_market_rollover_row(AsyncJsonlWriter& writer, const std::string& old_slug,
                                     const std::string& new_slug, const std::string& old_up,
                                     const std::string& new_up, const std::string& old_down,
                                     const std::string& new_down, double price_to_beat) {
  nlohmann::json row;
  row["schema_version"] = 1;
  row["source"] = "polymarket";
  row["event_type"] = "market_rollover";
  row["old_slug"] = old_slug;
  row["new_slug"] = new_slug;
  if (std::isfinite(price_to_beat)) {
    row["price_to_beat"] = price_to_beat;
  } else {
    row["price_to_beat"] = nullptr;
  }
  row["old_up_token_id"] = old_up;
  row["new_up_token_id"] = new_up;
  row["old_down_token_id"] = old_down;
  row["new_down_token_id"] = new_down;
  row["local_ts_mono_ns"] = ll::core::steady_ns();
  row["local_ts_wall_ms"] = ll::core::system_ms();
  writer.enqueue(std::move(row));
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, on_sig);
  std::signal(SIGTERM, on_sig);
  int exit_code = 0;

  if (argc < 3) {
    std::cerr << "usage: live_pipeline <spot_out.jsonl> <polymarket_out.jsonl> [options]\n"
                 "  spot_out.jsonl: Polymarket RTDS Chainlink (crypto_prices_chainlink btc/usd).\n"
                 "  options:\n"
                 "    --sigma S                   (hot path theo fallback; annualized, e.g. 0.7)\n"
                 "    --sigma-step-ms N           (hot path 1h realized resample step; default 1000)\n"
                 "    --edge-threshold X          (hot path print threshold; default 0.03)\n"
                 "    --poly-discover             (default: on; Gamma bucket + dual WS + rollover)\n"
                 "    --poly-rollover-web-ptb     warmup + rollover: event-page strike first;\n"
                 "                                fallback RTDS Chainlink (prefetch still RTDS-only)\n"
                 "    --poly-token TOKEN          Up clob token; requires --poly-event-slug + dual WS\n"
                 "    --poly-event-slug SLUG      Gamma btc-updown-5m-<epoch>; required without --poly-discover\n"
                 "    --poly-slug-prefix etc.     (accepted for compatibility; discovery unchanged)\n";
    return 2;
  }

  const std::string bin_out = argv[1];
  const std::string poly_out = argv[2];

  double sigma_fallback = 0.7;
  std::int64_t sigma_step_ms = 1000;
  double edge_threshold = 0.03;
  const bool spot_feed_chainlink = true;

  bool poly_discover = true;
  bool poly_rollover_web_ptb = false;
  std::string poly_manual_token;
  std::string poly_event_slug;

  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--poly-token" && i + 1 < argc) {
      poly_manual_token = argv[++i];
      poly_discover = false;
    } else if (a == "--poly-event-slug" && i + 1 < argc) {
      poly_event_slug = argv[++i];
    } else if (a == "--poly-discover") {
      poly_discover = true;
      poly_manual_token.clear();
      poly_event_slug.clear();
    } else if (a == "--poly-rollover-web-ptb") {
      poly_rollover_web_ptb = true;
    } else if ((a == "--poly-slug-prefix" || a == "--poly-slug-filter" || a == "--poly-rollover-prefix" ||
                a == "--poly-rollover-slug") &&
               i + 1 < argc) {
      ++i;
    } else if (a == "--sigma" && i + 1 < argc) {
      sigma_fallback = std::stod(argv[++i]);
    } else if (a == "--sigma-step-ms" && i + 1 < argc) {
      sigma_step_ms = static_cast<std::int64_t>(std::stoll(argv[++i]));
    } else if (a == "--edge-threshold" && i + 1 < argc) {
      edge_threshold = std::stod(argv[++i]);
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      return 2;
    }
  }

  if (!poly_discover) {
    if (poly_manual_token.empty() || poly_event_slug.empty()) {
      std::cerr << "[live_pipeline] --poly-token and --poly-event-slug required when not using discovery\n";
      return 2;
    }
  }

  ll::polymarket::BtcFiveMinuteBucketDiscovery initial_disc;
  std::int64_t active_epoch = -1;
  std::int64_t next_epoch = -1;
  std::string next_up_token;
  std::string next_down_token;
  std::string next_confirmed_slug;
  double next_prefetch_price_to_beat = std::numeric_limits<double>::quiet_NaN();
  bool next_ready = false;
  auto last_prefetch_mono = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  constexpr auto kPrefetchInterval = std::chrono::seconds(3);
  std::string current_slug;

  if (poly_discover) {
    std::string derr;
    std::string err_exact;
    const std::time_t wall_now = std::time(nullptr);
    const std::int64_t wall_now_s = static_cast<std::int64_t>(wall_now);
    const std::int64_t utc_floor = (wall_now_s / 300) * 300;
    const std::string slug_exact = "btc-updown-5m-" + std::to_string(utc_floor);
    bool discovered = false;
    if (ll::polymarket::discover_btc_updown_5m_for_exact_slug(slug_exact, initial_disc, &err_exact)) {
      std::int64_t ep = initial_disc.bucket_epoch_seconds;
      if (ep < 0 && !parse_epoch_from_confirmed_slug(initial_disc.confirmed_slug, &ep)) {
        ep = -1;
      }
      if (ep >= 0 && wall_now_s >= ep && wall_now_s < ep + 300) {
        discovered = true;
      }
    }
    if (!discovered) {
      if (!ll::polymarket::discover_active_btc_updown_5m_via_bucket(initial_disc, &derr)) {
        std::cerr << "[live_pipeline] polymarket bucket discovery failed: exact_slug_err=" << err_exact
                  << " scan_err=" << derr << "\n";
        return 1;
      }
    }
  } else {
    std::string derr;
    if (!ll::polymarket::discover_btc_updown_5m_for_exact_slug(poly_event_slug, initial_disc, &derr)) {
      std::cerr << "[live_pipeline] polymarket slug discovery failed: " << derr << "\n";
      return 1;
    }
    if (poly_manual_token != initial_disc.up_token_id) {
      std::cerr << "[live_pipeline] --poly-token must match Gamma Up token for --poly-event-slug\n";
      return 1;
    }
  }
  if (initial_disc.up_token_id.empty() || initial_disc.down_token_id.empty()) {
    std::cerr << "[live_pipeline] polymarket discovery missing up or down token id\n";
    return 1;
  }
  active_epoch = initial_disc.bucket_epoch_seconds;
  if (active_epoch < 0 &&
      !parse_epoch_from_confirmed_slug(initial_disc.confirmed_slug, &active_epoch)) {
    std::cerr << "[live_pipeline] polymarket bucket epoch unavailable from slug\n";
    return 1;
  }
  next_epoch = active_epoch + 300;
  current_slug = initial_disc.confirmed_slug;
  live_pipe_maybe_web_then_rtds_strike(initial_disc, poly_rollover_web_ptb, "[live_pipeline]");
  std::cerr << "[live_pipeline] polymarket discovered slug=" << initial_disc.confirmed_slug
            << " up_token=" << initial_disc.up_token_id << " down_token=" << initial_disc.down_token_id
            << " price_to_beat=";
  if (initial_disc.gamma_has_price_to_beat && live_pipe_disc_has_strike(initial_disc)) {
    std::cerr << initial_disc.price_to_beat << " (gamma_metadata)\n";
  } else if (initial_disc.strike_from_polymarket_web_event_page) {
    std::cerr << initial_disc.price_to_beat << " (polymarket_web_event_page)\n";
  } else if (initial_disc.strike_from_polymarket_rtds_chainlink) {
    std::cerr << initial_disc.price_to_beat << " (polymarket_rtds_chainlink)\n";
  } else {
    std::cerr << initial_disc.price_to_beat << " (unavailable)\n";
  }

  ll::telemetry::Pipeline tel;
  AsyncJsonlWriter bin_writer(bin_out);
  AsyncJsonlWriter poly_writer(poly_out);

  std::atomic<std::uint64_t> bin_seq{0};
  std::atomic<std::uint64_t> poly_seq{0};

  // Hot path counters (in-memory): useful for debugging/benchmarking without disk I/O.
  std::atomic<std::uint64_t> bin_hot_events{0};
  std::atomic<std::uint64_t> poly_hot_events{0};
  HotState hot;
  {
    std::lock_guard<std::mutex> hk(hot.mu);
    hot.slug = current_slug;
    hot.active_epoch = active_epoch;
    if (live_pipe_disc_has_strike(initial_disc)) {
      hot.have_K = true;
      hot.K = initial_disc.price_to_beat;
    } else {
      hot.have_K = false;
    }
  }

  ll::polymarket::WsFixedTokenQuoteFeed feed_up(&tel);
  ll::polymarket::WsFixedTokenQuoteFeed feed_down(&tel);

  std::cerr << "[live_pipeline][spot] Polymarket RTDS Chainlink btc/usd for hot S\n";

  auto maybe_print_signal = [&]() {
    // Called from callback threads; keep it small and rate-limited.
    constexpr std::int64_t kMinPrintEveryNs = 200 * 1000 * 1000;  // 200ms
    const auto now_ns = ll::core::steady_ns();

    // Snapshot minimal hot state under lock, then compute + paper-trade outside the lock.
    std::string slug;
    std::int64_t active_epoch = -1;
    std::int64_t bin_wall_ms = 0;
    double S = 0.0;
    double K = 0.0;
    double sigma_bucket = 0.0;
    bool have_sigma = false;
    double up_ask = 0.0, up_bid = 0.0, dn_ask = 0.0, dn_bid = 0.0;
    {
      std::lock_guard<std::mutex> lk(hot.mu);
      if (hot.active_epoch < 0 || !hot.have_K || !hot.have_bin || !hot.have_up || !hot.have_down) {
        return;
      }
      slug = hot.slug;
      active_epoch = hot.active_epoch;
      bin_wall_ms = hot.bin_wall_ms;
      S = hot.bin_mid;
      K = hot.K;
      have_sigma = hot.have_sigma;
      sigma_bucket = hot.sigma_bucket;
      up_ask = hot.up_ask;
      up_bid = hot.up_bid;
      dn_ask = hot.down_ask;
      dn_bid = hot.down_bid;
    }

    const std::int64_t exp_wall_ms = (active_epoch + 300) * 1000;
    const double rem_s = std::max(0.001, (exp_wall_ms - bin_wall_ms) / 1000.0);
    const double T_years = rem_s / (365.0 * 24.0 * 3600.0);
    const double sigma_use = have_sigma ? sigma_bucket : sigma_fallback;
    const double p_theo_up = digital_call_prob(S, K, T_years, sigma_use);
    const double p_theo_down = 1.0 - p_theo_up;

    const double edge_up = p_theo_up - up_ask;
    const double edge_down = p_theo_down - dn_ask;

    // Rate-limited logging (only for report/debug).
    {
      std::lock_guard<std::mutex> lk(hot.mu);
      if (hot.last_print_mono_ns != 0 && now_ns - hot.last_print_mono_ns < kMinPrintEveryNs) {
        return;
      }
      if (edge_up >= edge_threshold || edge_down >= edge_threshold) {
        hot.last_print_mono_ns = now_ns;
        std::cerr << "[hot_signal] slug=" << slug << " S=" << S << " K=" << K << " T_s=" << rem_s
                  << " sigma=" << sigma_use << " theo_up=" << p_theo_up << " up(ask)=" << up_ask
                  << " edge_up=" << edge_up << " down(ask)=" << dn_ask << " edge_down=" << edge_down
                  << "\n";
      }
    }
  };

  ll::polymarket::RtdsChainlinkFeed::instance().set_on_tick([&](std::int64_t ts_ms, double px) {
    bin_hot_events.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> hk(hot.mu);
      hot.have_bin = true;
      hot.bin_mid = px;
      hot.bin_wall_ms = ts_ms;

      hot.bin_hist.emplace_back(ts_ms, px);
      const std::int64_t cutoff = ts_ms - 3600'000;
      while (!hot.bin_hist.empty() && hot.bin_hist.front().first < cutoff) {
        hot.bin_hist.pop_front();
      }

      if (hot.active_epoch >= 0 && hot.have_K && !hot.have_sigma &&
          hot.bin_wall_ms >= hot.active_epoch * 1000) {
        const auto sigma =
            realized_vol_1h_resampled_from_hist(hot.bin_hist, hot.active_epoch * 1000, sigma_step_ms);
        if (std::isfinite(sigma) && sigma > 0) {
          hot.have_sigma = true;
          hot.sigma_bucket = sigma;
        } else {
          hot.have_sigma = false;
          hot.sigma_bucket = 0.0;
        }
      }
    }
    maybe_print_signal();

    const auto n = bin_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    nlohmann::json row;
    row["schema_version"] = 1;
    row["source"] = "polymarket_rtds_chainlink";
    row["symbol"] = "BTC/USD";
    row["payload_ts_ms"] = ts_ms;
    row["local_ts_mono_ns"] = ll::core::steady_ns();
    row["local_ts_wall_ms"] = ll::core::system_ms();
    row["seq"] = n;
    row["event_type"] = "chainlink";
    row["payload"] = {{"price", px}};
    bin_writer.enqueue(std::move(row));
  });
  ll::polymarket::RtdsChainlinkFeed::instance().ensure_started();

  feed_up.set_market_context(current_slug, active_epoch, "Up");
  feed_down.set_market_context(current_slug, active_epoch, "Down");
  feed_up.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
    poly_hot_events.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> hk(hot.mu);
      hot.slug = q.event_slug;
      hot.active_epoch = q.market_bucket_epoch >= 0 ? q.market_bucket_epoch : active_epoch;
      hot.have_up = true;
      hot.up_bid = q.best_bid;
      hot.up_ask = q.best_ask;
      hot.up_wall_ms = q.local_wall_ms;
    }
    maybe_print_signal();
    append_poly_quote_row(poly_writer, poly_seq, q);
  });
  feed_down.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
    poly_hot_events.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> hk(hot.mu);
      hot.slug = q.event_slug;
      hot.active_epoch = q.market_bucket_epoch >= 0 ? q.market_bucket_epoch : active_epoch;
      hot.have_down = true;
      hot.down_bid = q.best_bid;
      hot.down_ask = q.best_ask;
      hot.down_wall_ms = q.local_wall_ms;
    }
    maybe_print_signal();
    append_poly_quote_row(poly_writer, poly_seq, q);
  });
  std::string err_up;
  std::string err_dn;
  if (!feed_up.start(initial_disc.up_token_id, &err_up)) {
    std::cerr << "[live_pipeline] polymarket start (up) failed: " << err_up << "\n";
    exit_code = 1;
    g_stop.store(true, std::memory_order_release);
  } else if (!feed_down.start(initial_disc.down_token_id, &err_dn)) {
    std::cerr << "[live_pipeline] polymarket start (down) failed: " << err_dn << "\n";
    feed_up.stop();
    exit_code = 1;
    g_stop.store(true, std::memory_order_release);
  } else {
    ll::polymarket::RtdsChainlinkFeed::instance().ensure_started();
  }

  while (!g_stop.load(std::memory_order_acquire)) {
    if (poly_discover) {
      const auto now_mono = std::chrono::steady_clock::now();
      const std::time_t wall_now = std::time(nullptr);

      if (!next_ready && (now_mono - last_prefetch_mono >= kPrefetchInterval)) {
        last_prefetch_mono = now_mono;
        ll::polymarket::BtcFiveMinuteBucketDiscovery nd;
        const std::string slug = "btc-updown-5m-" + std::to_string(next_epoch);
        std::string perr;
        if (ll::polymarket::discover_btc_updown_5m_for_exact_slug(slug, nd, &perr)) {
          if (!nd.up_token_id.empty() && !nd.down_token_id.empty()) {
            next_up_token = nd.up_token_id;
            next_down_token = nd.down_token_id;
            next_confirmed_slug = nd.confirmed_slug;
            next_prefetch_price_to_beat =
                (nd.gamma_has_price_to_beat && nd.price_to_beat > 0.0)
                    ? nd.price_to_beat
                    : std::numeric_limits<double>::quiet_NaN();
            if (!std::isfinite(next_prefetch_price_to_beat) || next_prefetch_price_to_beat <= 0.0) {
              live_pipe_maybe_rtds_strike(nd, "[live_pipeline]");
              if (live_pipe_disc_has_strike(nd)) {
                next_prefetch_price_to_beat = nd.price_to_beat;
              }
            }
            next_ready = true;
            std::cerr << "[live_pipeline][prefetch] next slug=" << nd.confirmed_slug
                      << " up_token=" << next_up_token << " down_token=" << next_down_token
                      << " price_to_beat=" << nd.price_to_beat << "\n";
          }
        }
      }

      if (wall_now >= active_epoch + 300) {
        if (next_ready && wall_now >= next_epoch + 300) {
          next_ready = false;
          next_up_token.clear();
          next_down_token.clear();
          next_confirmed_slug.clear();
          next_prefetch_price_to_beat = std::numeric_limits<double>::quiet_NaN();
        }

        std::string new_up;
        std::string new_down;
        std::string new_slug;
        std::int64_t new_active = -1;
        double new_strike_ptb = std::numeric_limits<double>::quiet_NaN();
        ll::polymarket::BtcFiveMinuteBucketDiscovery rollover_disc;
        bool have_rollover_disc = false;

        if (next_ready) {
          new_up = next_up_token;
          new_down = next_down_token;
          new_slug = next_confirmed_slug.empty() ? ("btc-updown-5m-" + std::to_string(next_epoch))
                                                  : next_confirmed_slug;
          new_active = next_epoch;
          new_strike_ptb = next_prefetch_price_to_beat;
        } else {
          ll::polymarket::BtcFiveMinuteBucketDiscovery d2;
          std::string e2;
          const std::string slug_next = "btc-updown-5m-" + std::to_string(next_epoch);
          bool filled = false;
          if (ll::polymarket::discover_btc_updown_5m_for_exact_slug(slug_next, d2, &e2)) {
            std::int64_t ep = d2.bucket_epoch_seconds;
            if (ep < 0 && !parse_epoch_from_confirmed_slug(d2.confirmed_slug, &ep)) {
              ep = -1;
            }
            if (ep >= next_epoch) {
              new_up = d2.up_token_id;
              new_down = d2.down_token_id;
              new_slug = d2.confirmed_slug;
              new_active = ep;
              new_strike_ptb = d2.price_to_beat;
              filled = true;
              rollover_disc = d2;
              have_rollover_disc = true;
            }
          }
          if (!filled &&
              ll::polymarket::discover_active_btc_updown_5m_via_bucket(d2, &e2) &&
              d2.bucket_epoch_seconds >= next_epoch) {
            new_up = d2.up_token_id;
            new_down = d2.down_token_id;
            new_slug = d2.confirmed_slug;
            if (d2.bucket_epoch_seconds >= 0) {
              new_active = d2.bucket_epoch_seconds;
            } else if (!parse_epoch_from_confirmed_slug(d2.confirmed_slug, &new_active)) {
              std::cerr << "[live_pipeline][rollover] bucket fallback missing epoch: " << e2 << "\n";
            }
            new_strike_ptb = d2.price_to_beat;
            filled = true;
            rollover_disc = d2;
            have_rollover_disc = true;
          }
          if (!filled) {
            std::cerr << "[live_pipeline][rollover] discovery failed (exact then bucket): " << e2 << "\n";
          }
        }

        if (next_ready && (!std::isfinite(new_strike_ptb) || new_strike_ptb <= 0.0)) {
          ll::polymarket::BtcFiveMinuteBucketDiscovery pf;
          std::string perr;
          if (ll::polymarket::discover_btc_updown_5m_for_exact_slug(new_slug, pf, &perr)) {
            live_pipe_maybe_web_then_rtds_strike(pf, poly_rollover_web_ptb, "[live_pipeline]");
            if (live_pipe_disc_has_strike(pf)) {
              new_strike_ptb = pf.price_to_beat;
            }
          }
        } else if (have_rollover_disc && !rollover_disc.gamma_has_price_to_beat) {
          live_pipe_maybe_web_then_rtds_strike(rollover_disc, poly_rollover_web_ptb, "[live_pipeline]");
          if (live_pipe_disc_has_strike(rollover_disc)) {
            new_strike_ptb = rollover_disc.price_to_beat;
          }
        }

        const bool have_strike_k = std::isfinite(new_strike_ptb) && new_strike_ptb > 0.0;
        if (!new_up.empty() && !new_down.empty() && new_active >= 0 && !new_slug.empty()) {
          const std::string old_slug = current_slug;
          const std::string old_up = feed_up.token_id();
          const std::string old_down = feed_down.token_id();

          std::string err_up;
          std::string err_dn;
          if (!feed_up.switch_token(new_up, &err_up)) {
            std::cerr << "[live_pipeline][rollover] switch_token (up) failed: " << err_up << "\n";
          } else if (!feed_down.switch_token(new_down, &err_dn)) {
            std::cerr << "[live_pipeline][rollover] switch_token (down) failed: " << err_dn << "\n";
          } else {
            feed_up.set_market_context(new_slug, new_active, "Up");
            feed_down.set_market_context(new_slug, new_active, "Down");
            {
              std::lock_guard<std::mutex> hk(hot.mu);
              hot.slug = new_slug;
              hot.active_epoch = new_active;
              if (have_strike_k) {
                hot.have_K = true;
                hot.K = new_strike_ptb;
              } else {
                hot.have_K = false;
              }
              hot.have_sigma = false;
              hot.sigma_bucket = 0.0;
              hot.last_print_mono_ns = 0;
            }
            append_poly_market_rollover_row(poly_writer, old_slug, new_slug, old_up, new_up, old_down,
                                             new_down, new_strike_ptb);
            active_epoch = new_active;
            next_epoch = active_epoch + 300;
            current_slug = new_slug;
            next_ready = false;
            next_up_token.clear();
            next_down_token.clear();
            next_confirmed_slug.clear();
            next_prefetch_price_to_beat = std::numeric_limits<double>::quiet_NaN();
            std::cerr << "[live_pipeline][rollover] active_epoch=" << active_epoch << " slug=" << new_slug
                      << " price_to_beat=";
            if (have_strike_k) {
              std::cerr << new_strike_ptb;
            } else {
              std::cerr << "(strike_unavailable)";
            }
            std::cerr << "\n";
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  feed_up.stop();
  feed_down.stop();

  bin_writer.stop_and_flush();
  poly_writer.stop_and_flush();
  std::cerr << "[live_pipeline] hot_counts spot=" << bin_hot_events.load(std::memory_order_relaxed)
            << " poly=" << poly_hot_events.load(std::memory_order_relaxed) << "\n";
  std::cerr << "telemetry: " << tel.summary() << "\n";
  return exit_code;
}
