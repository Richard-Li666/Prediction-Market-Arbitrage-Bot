#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <cstdint>
#include <deque>
#include <memory>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "binance/stream_client.hpp"
#include "binance/stream_env.hpp"
#include "core/clock.hpp"
#include "execution/executor.hpp"
#include "polymarket/bucket_market_discovery.hpp"
#include "polymarket/polymarket_web_ptb.hpp"
#include "polymarket/rtds_chainlink_feed.hpp"
#include "polymarket/rtds_chainlink_strike.hpp"
#include "polymarket/ws_fixed_token_feed.hpp"
#include "replay/timeline_merge.hpp"
#include "signals/engine.hpp"
#include "sim/latency_backtest.hpp"
#include "telemetry/pipeline.hpp"

#include "core/sync_cerr.hpp"
namespace {

constexpr std::int64_t kRtdsStrikeTimeoutMs = 15000;
/// Accept Chainlink RTDS tick if `|payload_ts - event_start|` ≤ this (Polymarket crypto stream has no long history buffer).
constexpr std::int64_t kRtdsStrikeMaxSkewMs = 180000;

bool bucket_disc_has_strike(const ll::polymarket::BtcFiveMinuteBucketDiscovery& d) {
  return std::isfinite(d.price_to_beat) && d.price_to_beat > 0.0;
}

void maybe_fill_strike_rtds(ll::polymarket::BtcFiveMinuteBucketDiscovery& disc, const char* log_tag) {
  if (disc.gamma_has_price_to_beat && bucket_disc_has_strike(disc)) {
    return;
  }
  std::string rerr;
  if (ll::polymarket::fill_strike_from_polymarket_rtds_chainlink(disc, kRtdsStrikeTimeoutMs,
                                                                 kRtdsStrikeMaxSkewMs, &rerr)) {
    ll::io::SyncCerrLock _;
    std::cerr << log_tag << "[rtds_chainlink] strike=" << disc.price_to_beat << " target_wall_ms="
              << disc.event_start_wall_ms << "\n";
    return;
  }
  {
    ll::io::SyncCerrLock _;
    std::cerr << log_tag << "[rtds_chainlink] strike unavailable: " << rerr << "\n";
  }
}

void maybe_fill_strike_web_then_rtds(ll::polymarket::BtcFiveMinuteBucketDiscovery& disc, bool try_web_first,
                                     const char* log_tag) {
  if (disc.gamma_has_price_to_beat && bucket_disc_has_strike(disc)) {
    return;
  }
  if (try_web_first) {
    std::string werr;
    if (ll::polymarket::fill_strike_from_polymarket_web_event_page(disc, &werr)) {
      ll::io::SyncCerrLock _;
      std::cerr << log_tag << "[web_ptb] strike=" << disc.price_to_beat << " slug=" << disc.confirmed_slug
                << "\n";
      return;
    }
    {
      ll::io::SyncCerrLock _;
      std::cerr << log_tag << "[web_ptb] unavailable: " << werr << "\n";
    }
  }
  maybe_fill_strike_rtds(disc, log_tag);
}

/// Best-effort .env loader for the C++ process.
/// Supports KEY=VALUE and optional `export KEY=VALUE`.
/// Only sets a variable if it is not already present in the environment.
void load_dotenv(const char* path = ".env") {
  std::ifstream f(path);
  if (!f.is_open()) return;
  std::string raw;
  while (std::getline(f, raw)) {
    // trim leading whitespace
    auto beg = raw.find_first_not_of(" \t\r\n");
    if (beg == std::string::npos) continue;
    std::string line = raw.substr(beg);
    if (line.empty() || line[0] == '#') continue;
    if (line.rfind("export ", 0) == 0) line = line.substr(7);
    auto eq = line.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    // strip trailing whitespace / CR
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
    // strip optional surrounding double quotes
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
      val = val.substr(1, val.size() - 2);
    }
    if (key.empty()) continue;
    // only set if not already present (0 = don't overwrite)
    ::setenv(key.c_str(), val.c_str(), 0);
  }
}

std::atomic<bool> g_stop{false};
void on_sig(int) { g_stop = true; }

/// `q` in [0,1]; `v` sorted ascending (copy).
static double percentile_sorted_copy(std::vector<std::int64_t> v, double q) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(v.begin(), v.end());
  q = std::max(0.0, std::min(1.0, q));
  const double pos = q * static_cast<double>(v.size() - 1);
  const std::size_t i = static_cast<std::size_t>(pos);
  const std::size_t j = (i + 1 < v.size()) ? i + 1 : i;
  const double t = pos - static_cast<double>(i);
  return (1.0 - t) * static_cast<double>(v[i]) + t * static_cast<double>(v[j]);
}

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
  static_cast<std::string*>(ud)->append(ptr, size * nmemb);
  return size * nmemb;
}

/// REST base for `/api/v3/klines` — align with WebSocket host (`--host` / `LL_BINANCE_WS_HOST`).
/// Non‑US IPs often get HTTP 451 from api.binance.com; use US WS + matching REST (or set LL_BINANCE_REST_BASE).
std::string binance_klines_rest_base(const ll::binance::StreamClientConfig& bin_cfg) {
  if (const char* b = std::getenv("LL_BINANCE_REST_BASE")) {
    if (b[0] != '\0') {
      std::string s(b);
      while (!s.empty() && (s.back() == '/' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
      }
      return s;
    }
  }
  if (bin_cfg.ws_host.find("binance.us") != std::string::npos) {
    return "https://api.binance.us";
  }
  return "https://api.binance.com";
}

/// Fetch past 1h of Binance 1s klines and populate bin_hist for immediate sigma computation.
void prefill_bin_hist(std::deque<std::pair<std::int64_t, double>>& hist,
                      const ll::binance::StreamClientConfig& bin_cfg) {
  const std::string url = binance_klines_rest_base(bin_cfg) +
                          "/api/v3/klines?symbol=BTCUSDT&interval=1s&limit=3600";
  CURL* curl = curl_easy_init();
  if (!curl) {
    { ll::io::SyncCerrLock _; std::cerr << "[prefill] curl_easy_init failed\n"; }
    return;
  }
  std::string body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "leadlag/1.0");
  CURLcode res = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);
  if (res != CURLE_OK || code < 200 || code >= 300) {
    {
      ll::io::SyncCerrLock _;
      std::cerr << "[prefill] Binance klines fetch failed: curl=" << static_cast<int>(res)
                << " http=" << code << "\n";
    }
    return;
  }
  try {
    const auto arr = nlohmann::json::parse(body);
    if (!arr.is_array()) return;
    std::size_t count = 0;
    for (const auto& k : arr) {
      if (!k.is_array() || k.size() < 5) continue;
      // kline: [open_time, open, high, low, close, ...]
      const std::int64_t open_ms = k[0].get<std::int64_t>();
      const double open = std::stod(k[1].get<std::string>());
      const double close = std::stod(k[4].get<std::string>());
      const double mid = 0.5 * (open + close);
      hist.emplace_back(open_ms, mid);
      ++count;
    }
    { ll::io::SyncCerrLock _; std::cerr << "[prefill] loaded " << count << " Binance 1s klines into bin_hist\n"; }
  } catch (const std::exception& e) {
    { ll::io::SyncCerrLock _; std::cerr << "[prefill] parse error: " << e.what() << "\n"; }
  }
}

class TradeJsonlWriter {
 public:
  explicit TradeJsonlWriter(std::string path) : path_(std::move(path)) {
    try {
      std::filesystem::path p(path_);
      if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
      }
    } catch (...) {
      // best-effort
    }
    out_.open(path_, std::ios::out | std::ios::trunc);
    ok_ = static_cast<bool>(out_);
    if (!ok_) {
      { ll::io::SyncCerrLock _; std::cerr << "[paper_trader] cannot open trades jsonl for write: " << path_ << "\n"; }
    }
  }

  void append(const nlohmann::json& row) {
    if (!ok_) return;
    std::lock_guard<std::mutex> lk(mu_);
    out_ << row.dump() << "\n";
    out_.flush();
  }

  const std::string& path() const { return path_; }
  bool ok() const { return ok_; }

 private:
  std::string path_;
  std::ofstream out_;
  bool ok_{false};
  std::mutex mu_;
};

struct HotState {
  std::mutex mu;
  std::int64_t active_epoch{-1};
  std::string slug;

  bool have_bin{false};
  double bin_mid{0.0};
  std::int64_t bin_wall_ms{0};
  /// Local wall at last spot update (Chainlink: ingest `system_ms()`; Binance: tick `local_wall_ms`).
  /// Used for BUY entry window vs bucket start; `bin_wall_ms` may be Chainlink payload time for theo T.
  std::int64_t bin_entry_clock_wall_ms{0};

  std::deque<std::pair<std::int64_t, double>> bin_hist;

  bool have_K{false};
  double K{0.0};

  double sigma_bucket{0.0};
  bool have_sigma{false};

  bool have_up{false};
  double up_bid{0.0};
  double up_ask{0.0};

  bool have_down{false};
  double down_bid{0.0};
  double down_ask{0.0};

  /// Mono time when `theo-ask >= entry` (Up) or `theo-ask >= entry` (Down) first became true; -1 if not armed.
  /// Aligns with `data/real_backtest.ipynb` EDGE_PERSIST_MS (continuous edge in steady time).
  std::int64_t edge_up_ok_since_mono_ns{-1};
  std::int64_t edge_dn_ok_since_mono_ns{-1};
};

static inline double clamp01(double x) { return (x < 0.0) ? 0.0 : (x > 1.0 ? 1.0 : x); }

static inline double normal_cdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

static inline double digital_call_prob(double S, double K, double T_years, double sigma, double r) {
  if (T_years <= 0.0) {
    return (S > K) ? 1.0 : 0.0;
  }
  if (sigma <= 0.0) {
    return (S > K) ? 1.0 : 0.0;
  }
  const double vsqrt = sigma * std::sqrt(T_years);
  const double d2 = (std::log(S / K) + (r - 0.5 * sigma * sigma) * T_years) / vsqrt;
  const double disc = std::exp(-r * T_years);
  return clamp01(disc * normal_cdf(d2));
}

static inline double realized_vol_1h_resampled_from_hist(
    const std::deque<std::pair<std::int64_t, double>>& hist, std::int64_t now_wall_ms,
    std::int64_t step_ms) {
  if (hist.size() < 3) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const std::int64_t start_ms = now_wall_ms - 3600 * 1000;
  std::vector<double> mids;
  mids.reserve(static_cast<std::size_t>(3600'000 / std::max<std::int64_t>(1, step_ms)) + 4);

  // Resample via last observation carry-forward.
  std::size_t idx = 0;
  while (idx + 1 < hist.size() && hist[idx].first < start_ms) {
    ++idx;
  }
  double last_mid = hist[idx].second;
  for (std::int64_t t = start_ms; t <= now_wall_ms; t += step_ms) {
    while (idx + 1 < hist.size() && hist[idx + 1].first <= t) {
      ++idx;
      last_mid = hist[idx].second;
    }
    mids.push_back(last_mid);
  }
  if (mids.size() < 10) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  std::vector<double> rets;
  rets.reserve(mids.size() - 1);
  for (std::size_t i = 1; i < mids.size(); ++i) {
    const double r = std::log(mids[i] / mids[i - 1]);
    if (std::isfinite(r)) {
      rets.push_back(r);
    }
  }
  if (rets.size() < 10) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double mean = 0.0;
  for (double x : rets) mean += x;
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

static inline std::vector<double> resample_mids_from_hist(
    const std::deque<std::pair<std::int64_t, double>>& hist, std::int64_t now_wall_ms,
    std::int64_t step_ms) {
  std::vector<double> mids;
  if (hist.size() < 3) return mids;
  const std::int64_t start_ms = now_wall_ms - 3600 * 1000;
  mids.reserve(static_cast<std::size_t>(3600'000 / std::max<std::int64_t>(1, step_ms)) + 4);
  std::size_t idx = 0;
  while (idx + 1 < hist.size() && hist[idx].first < start_ms) ++idx;
  double last_mid = hist[idx].second;
  for (std::int64_t t = start_ms; t <= now_wall_ms; t += step_ms) {
    while (idx + 1 < hist.size() && hist[idx + 1].first <= t) {
      ++idx;
      last_mid = hist[idx].second;
    }
    mids.push_back(last_mid);
  }
  return mids;
}

static inline double poly_taker_fee(double notional_usdc, double price, double fee_rate) {
  // Polymarket (crypto taker) fee:
  // fee = C * feeRate * p * (1-p), rounded to 5 decimals, min 0.00001
  // Here C is notional in USDC (e.g., qty * price).
  double fee = notional_usdc * fee_rate * price * (1.0 - price);
  fee = std::round(fee * 1e5) / 1e5;
  return (fee >= 1e-5) ? fee : 0.0;
}

bool parse_epoch_from_confirmed_slug(const std::string& slug, std::int64_t* out_epoch) {
  static constexpr char kPrefix[] = "btc-updown-5m-";
  constexpr std::size_t plen = sizeof(kPrefix) - 1;
  if (slug.size() <= plen) return false;
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

int run_live_impl(bool live_execution, int argc, char** argv) {
  load_dotenv();

  std::signal(SIGINT, on_sig);
  std::signal(SIGTERM, on_sig);

  const std::string src_tag = live_execution ? "live_trader" : "paper_trader";
  const char* log_pfx = live_execution ? "[live]" : "[paper]";

  if (live_execution) {
    const char* rc = std::getenv("POLY_REQUIRE_CONFIRM");
    if (rc && rc[0]) {
      ll::io::SyncCerrLock _;
      std::cerr << "[live_trader] warning: POLY_REQUIRE_CONFIRM is set; each submit may be refused. "
                   "Unset it for automated --strategy mode.\n";
    }
  }

  std::string out_prefix = live_execution ? "live" : "paper";
  bool disable_rollover_force_sell = false;
  if (const char* v = std::getenv("POLY_DISABLE_ROLLOVER_FORCE_SELL")) {
    if (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y') {
      disable_rollover_force_sell = true;
    }
  }

  // Defaults: align with data/backtest.ipynb cell 5–6 (override via CLI).
  const double r = 0.035;
  const double sigma_min = 0.05;  // guardrail: ignore unrealistically tiny realized sigma
  const double sigma_max = 5.0;   // guardrail: ignore absurd spikes
  double sigma_fallback = 0.15;
  std::int64_t sigma_step_ms = 300;
  /// Prefer GARCH(1,1) via poly_daemon (`arch`) at bucket open; fallback to realized vol on failure.
  /// Default false matches data/backtest.ipynb SIGMA_MODE=\"realized\".
  bool sigma_try_garch = false;

  double initial_cash = 100.0;
  double risk_frac = 0.01;
  double entry = 0.15;  // theo - ask; align with data/backtest.ipynb ENTRY_DELTA_EXEC
  /// SELL when mid >= theo - close_eps (default 0 matches notebook: mid >= theo).
  double close_eps = 0.0;
  /// BUY only when local-wall seconds since bucket start in [min, max] (aligns with backtest.ipynb cell 10).
  bool use_entry_elapsed_window = true;
  double entry_elapsed_min_sec = 250.0;
  double entry_elapsed_max_sec = 298.0;
  /// BUY: `theo-ask >= entry` must hold continuously for this many ms (steady clock); 0 = off (legacy).
  /// Matches `data/real_backtest.ipynb` EDGE_PERSIST_MS.
  int edge_persist_ms = 1000;
  int lat_ms = live_execution ? 0 : 100;  // data/backtest.ipynb LAT_MS
  double fee_rate = 0.075;                // POLY_TAKER_FEE_RATE in backtest.ipynb
  double max_loss_usd = std::numeric_limits<double>::infinity();
  /// If > 0, each entry BUY uses this USDC notional (still capped by affordable cash). Otherwise use --risk-frac.
  double fixed_spend_usd = 0.0;
  /// BUY only when chosen outcome's best ask is in [buy_ask_min, buy_ask_max] (default [0, 0.2]; ask must be > 0).
  bool use_buy_ask_range = true;
  double buy_ask_min = 0.0;
  double buy_ask_max = 0.2;

  bool poly_discover = true;
  bool poly_rollover_web_ptb = false;
  std::string poly_manual_token;
  std::string poly_event_slug;
  std::size_t poly_parse_workers = 0;
  /// When true, spot `S` + realized-vol history come from Polymarket RTDS Chainlink (`btc/usd`), not Binance.
  bool spot_feed_chainlink = true;
  /// Consumer-thread spot path timing + queue depth (stderr + periodic summary).
  bool spot_latency_monitor = false;
  double spot_latency_report_sec = 5.0;
  if (const char* slm = std::getenv("LL_SPOT_LATENCY_MONITOR")) {
    if (slm[0] == '1' || slm[0] == 't' || slm[0] == 'T' || slm[0] == 'y' || slm[0] == 'Y') {
      spot_latency_monitor = true;
    }
  }

  ll::binance::StreamClientConfig bin_cfg;
  ll::binance::apply_stream_env_overrides(bin_cfg);

  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--stream" && i + 1 < argc) {
      const std::string m = argv[++i];
      if (m == "trade") {
        bin_cfg.stream_path = "/ws/btcusdt@trade";
      } else if (m == "bookTicker") {
        bin_cfg.stream_path = "/ws/btcusdt@bookTicker";
      } else {
        { ll::io::SyncCerrLock _; std::cerr << "--stream must be trade or bookTicker\n"; }
        return 2;
      }
    } else if (a == "--binance-stream" && i + 1 < argc) {
      const std::string m = argv[++i];
      if (m == "trade") {
        bin_cfg.stream_path = "/ws/btcusdt@trade";
      } else if (m == "bookTicker") {
        bin_cfg.stream_path = "/ws/btcusdt@bookTicker";
      } else {
        { ll::io::SyncCerrLock _; std::cerr << "--binance-stream must be trade or bookTicker\n"; }
        return 2;
      }
    } else if (a == "--host" && i + 1 < argc) {
      bin_cfg.ws_host = argv[++i];
    } else if (a == "--port" && i + 1 < argc) {
      bin_cfg.ws_port = std::stoi(argv[++i]);
    } else if (a == "--parse-workers" && i + 1 < argc) {
      bin_cfg.parse_workers = std::stoi(argv[++i]);
    } else if (a == "--poly-parse-workers" && i + 1 < argc) {
      poly_parse_workers = static_cast<std::size_t>(std::stoul(argv[++i]));
    } else if (a == "--spot-feed" && i + 1 < argc) {
      const std::string m = argv[++i];
      if (m == "chainlink" || m == "cl" || m == "rtds") {
        spot_feed_chainlink = true;
      } else if (m == "binance" || m == "bin") {
        spot_feed_chainlink = false;
      } else {
        { ll::io::SyncCerrLock _; std::cerr << "--spot-feed must be chainlink or binance\n"; }
        return 2;
      }
    } else if (a == "--out-prefix" && i + 1 < argc) {
      out_prefix = argv[++i];
    } else if (a == "--sigma" && i + 1 < argc) {
      sigma_fallback = std::stod(argv[++i]);
    } else if (a == "--sigma-step-ms" && i + 1 < argc) {
      sigma_step_ms = std::stoll(argv[++i]);
    } else if (a == "--sigma-model" && i + 1 < argc) {
      const std::string m = argv[++i];
      if (m == "garch" || m == "GARCH") {
        sigma_try_garch = true;
      } else if (m == "realized" || m == "rv") {
        sigma_try_garch = false;
      } else {
        { ll::io::SyncCerrLock _; std::cerr << "--sigma-model must be garch or realized\n"; }
        return 2;
      }
    } else if (a == "--initial-cash" && i + 1 < argc) {
      initial_cash = std::stod(argv[++i]);
    } else if (a == "--risk-frac" && i + 1 < argc) {
      risk_frac = std::stod(argv[++i]);
    } else if (a == "--entry" && i + 1 < argc) {
      entry = std::stod(argv[++i]);
    } else if (a == "--close" && i + 1 < argc) {
      close_eps = std::stod(argv[++i]);
    } else if (a == "--lat-ms" && i + 1 < argc) {
      lat_ms = std::stoi(argv[++i]);
    } else if (a == "--fee-rate" && i + 1 < argc) {
      fee_rate = std::stod(argv[++i]);
    } else if (a == "--max-loss-usd" && i + 1 < argc) {
      max_loss_usd = std::stod(argv[++i]);
    } else if (a == "--fixed-spend-usd" && i + 1 < argc) {
      fixed_spend_usd = std::stod(argv[++i]);
      if (fixed_spend_usd < 0.0) {
        { ll::io::SyncCerrLock _; std::cerr << "--fixed-spend-usd must be >= 0\n"; }
        return 2;
      }
    } else if (a == "--no-entry-elapsed-window") {
      use_entry_elapsed_window = false;
    } else if (a == "--entry-elapsed-min-sec" && i + 1 < argc) {
      entry_elapsed_min_sec = std::stod(argv[++i]);
    } else if (a == "--entry-elapsed-max-sec" && i + 1 < argc) {
      entry_elapsed_max_sec = std::stod(argv[++i]);
    } else if (a == "--edge-persist-ms" && i + 1 < argc) {
      edge_persist_ms = std::stoi(argv[++i]);
      if (edge_persist_ms < 0) {
        { ll::io::SyncCerrLock _; std::cerr << "--edge-persist-ms must be >= 0 (0 disables)\n"; }
        return 2;
      }
    } else if (a == "--buy-ask-min" && i + 1 < argc) {
      buy_ask_min = std::stod(argv[++i]);
    } else if (a == "--buy-ask-max" && i + 1 < argc) {
      buy_ask_max = std::stod(argv[++i]);
    } else if (a == "--no-buy-ask-range") {
      use_buy_ask_range = false;
    } else if (a == "--poly-token" && i + 1 < argc) {
      poly_discover = false;
      poly_manual_token = argv[++i];
    } else if (a == "--poly-event-slug" && i + 1 < argc) {
      poly_event_slug = argv[++i];
    } else if (a == "--poly-discover") {
      poly_discover = true;
    } else if (a == "--poly-rollover-web-ptb") {
      poly_rollover_web_ptb = true;
    } else if (a == "--disable-rollover-force-sell") {
      disable_rollover_force_sell = true;
    } else if (a == "--spot-latency-monitor") {
      spot_latency_monitor = true;
    } else if (a == "--spot-latency-report-sec" && i + 1 < argc) {
      spot_latency_report_sec = std::stod(argv[++i]);
      if (spot_latency_report_sec < 0.25) {
        { ll::io::SyncCerrLock _; std::cerr << "--spot-latency-report-sec must be >= 0.25\n"; }
        return 2;
      }
    } else if (a == "--help") {
      ll::io::SyncCerrLock _;
      std::cerr << "usage: paper_trader --live [options]   |   live_trader --strategy [options]\n"
                   "  --initial-cash X\n"
                   "  --risk-frac F              (default 0.01)\n"
                   "  --entry X                  (theo-ask; default 0.15 per backtest.ipynb)\n"
                   "  --close X                  (SELL when mid >= theo - X; default 0)\n"
                   "  --lat-ms N                 (BUY/SELL delay; paper default 100 per backtest.ipynb; live 0)\n"
                   "  --fee-rate R               (default 0.075 per backtest.ipynb)\n"
                   "  --max-loss-usd L           stop opening new BUY when mark-to-market loss >= L\n"
                   "                             (vs --initial-cash baseline; default: no cap)\n"
                   "  --fixed-spend-usd X        each BUY uses X USDC notional (overrides --risk-frac);\n"
                   "                             still capped by cash; Polymarket min ~$1 applies\n"
                   "  --entry-elapsed-min-sec S  (with max; default 250; local wall sec since bucket start,\n"
                   "                             same idea as backtest.ipynb cell 10 histogram)\n"
                   "  --entry-elapsed-max-sec S  (default 298; inclusive)\n"
                   "  --edge-persist-ms N        BUY: theo-ask>=entry must hold N ms steady time (default 1000;\n"
                   "                             0 off). Matches data/real_backtest.ipynb EDGE_PERSIST_MS.\n"
                   "  --no-entry-elapsed-window  allow BUY any time in bucket (disables the above)\n"
                   "  --buy-ask-min P            (with max; default 0; BUY only if best ask in [min, max])\n"
                   "  --buy-ask-max P            (default 0.2)\n"
                   "  --no-buy-ask-range         allow BUY at any ask (restores min-ask 0.02 guard only)\n"
                   "  --sigma S                  (fallback when bucket sigma missing)\n"
                   "  --sigma-step-ms N          (resample step for GARCH input + realized fallback)\n"
                   "  --sigma-model garch|realized   (default realized per backtest.ipynb; garch via poly_daemon)\n"
                   "  --spot-feed chainlink|binance   spot price for theo S + vol history (default: chainlink RTDS)\n"
                   "  --host/--port/--parse-workers/--stream ... (binance; only if --spot-feed binance)\n"
                   "  --poly-parse-workers N     (polymarket json parse workers; default 0)\n"
                   "  --out-prefix NAME          (under data/: *_trades.jsonl, *_series.jsonl;\n"
                   "                             with --spot-feed chainlink also *_chainlink.jsonl ticks)\n"
                   "  --poly-token TOKEN         (manual fixed token; pair with --poly-event-slug; dual WS)\n"
                   "  --poly-event-slug SLUG     required with --poly-token (Gamma btc-updown-5m-<epoch> slug)\n"
                   "  --poly-rollover-web-ptb    warmup + each rollover: strike from polymarket.com event page\n"
                   "                             first; fallback RTDS Chainlink if fetch fails\n"
                   "  --disable-rollover-force-sell  do not auto FORCE_SELL on rollover\n"
                   "                               (also env POLY_DISABLE_ROLLOVER_FORCE_SELL=1)\n"
                   "  --spot-latency-monitor       log spot handler latency + queue depth (stderr)\n"
                   "                               (also env LL_SPOT_LATENCY_MONITOR=1)\n"
                   "  --spot-latency-report-sec S  summary interval (default 5)\n";
      return 0;
    } else {
      { ll::io::SyncCerrLock _; std::cerr << "unknown arg: " << a << "\n"; }
      return 2;
    }
  }

  if (use_entry_elapsed_window && entry_elapsed_min_sec > entry_elapsed_max_sec) {
    { ll::io::SyncCerrLock _; std::cerr << "--entry-elapsed-min-sec must be <= --entry-elapsed-max-sec\n"; }
    return 2;
  }

  if (use_buy_ask_range) {
    if (!std::isfinite(buy_ask_min) || !std::isfinite(buy_ask_max) || buy_ask_min < 0.0 || buy_ask_max > 1.0 ||
        buy_ask_min > buy_ask_max) {
      { ll::io::SyncCerrLock _; std::cerr << "--buy-ask-min/--buy-ask-max invalid (expect 0<=min<=max<=1)\n"; }
      return 2;
    }
  }

  const std::int64_t edge_persist_ns =
      edge_persist_ms > 0 ? static_cast<std::int64_t>(edge_persist_ms) * 1'000'000LL : 0LL;

#ifndef LL_ENABLE_GARCH_DAEMON
  if (sigma_try_garch) {
    ll::io::SyncCerrLock _;
    std::cerr << "[" << src_tag << "] GARCH disabled in this build; using realized vol only "
                 "(configure with -DBUILD_DAEMON_GARCH=ON or -DBUILD_LIVE_TRADER=ON)\n";
  }
#endif

  if (!poly_discover) {
    if (poly_event_slug.empty()) {
      { ll::io::SyncCerrLock _; std::cerr << "--poly-token requires --poly-event-slug for Gamma priceToBeat\n"; }
      return 2;
    }
  }

  // Always record live trades/series locally (truncate on start).
  const std::string trades_path = "data/" + out_prefix + "_trades.jsonl";
  const std::string series_path = "data/" + out_prefix + "_series.jsonl";
  TradeJsonlWriter trades_writer(trades_path);
  if (trades_writer.ok()) {
    { ll::io::SyncCerrLock _; std::cerr << "[" << src_tag << "] recording trades to " << trades_writer.path() << " (truncated)\n"; }
  }
  TradeJsonlWriter series_writer(series_path);
  if (series_writer.ok()) {
    { ll::io::SyncCerrLock _; std::cerr << "[" << src_tag << "] recording series to " << series_writer.path() << " (truncated)\n"; }
  }
  {
    ll::io::SyncCerrLock _;
    if (use_entry_elapsed_window) {
      std::cerr << "[" << src_tag << "] BUY entry window (local wall vs bucket): [" << entry_elapsed_min_sec << ", "
                << entry_elapsed_max_sec << "] s (backtest.ipynb cell 10)\n";
    } else {
      std::cerr << "[" << src_tag << "] BUY entry window: disabled\n";
    }
    if (use_buy_ask_range) {
      std::cerr << "[" << src_tag << "] BUY ask range: [" << buy_ask_min << ", " << buy_ask_max << "] (ask>0)\n";
    } else {
      std::cerr << "[" << src_tag << "] BUY ask range: disabled (min ask 0.02 for entry)\n";
    }
    if (edge_persist_ns > 0) {
      std::cerr << "[" << src_tag << "] BUY edge persist: " << edge_persist_ms
                << " ms steady (real_backtest.ipynb EDGE_PERSIST_MS)\n";
    } else {
      std::cerr << "[" << src_tag << "] BUY edge persist: off\n";
    }
  }
  std::unique_ptr<TradeJsonlWriter> chainlink_ticks_writer;
  std::atomic<std::uint64_t> chainlink_tick_seq{0};
  if (spot_feed_chainlink) {
    const std::string cl_path = "data/" + out_prefix + "_chainlink.jsonl";
    chainlink_ticks_writer = std::make_unique<TradeJsonlWriter>(cl_path);
    if (chainlink_ticks_writer->ok()) {
      { ll::io::SyncCerrLock _; std::cerr << "[" << src_tag << "] recording Chainlink ticks to " << chainlink_ticks_writer->path() << " (truncated)\n"; }
    }
  }
  std::int64_t last_series_mono_ns = 0;
  constexpr std::int64_t kSeriesEveryNs = 200 * 1000 * 1000;  // 200ms

  ll::telemetry::Pipeline tel;
  std::unique_ptr<ll::binance::StreamClient> bin_client;
  if (!spot_feed_chainlink) {
    bin_client = std::make_unique<ll::binance::StreamClient>(&tel);
  }
  ll::polymarket::WsFixedTokenQuoteFeed feed_up(&tel);
  ll::polymarket::WsFixedTokenQuoteFeed feed_down(&tel);
  feed_up.set_parse_workers(poly_parse_workers);
  feed_down.set_parse_workers(poly_parse_workers);

  HotState hot;
  if (!spot_feed_chainlink) {
    prefill_bin_hist(hot.bin_hist, bin_cfg);
  }

  ll::execution::LiveExecutor live_ex;

  {
    ll::io::SyncCerrLock _;
    if (!spot_feed_chainlink) {
      std::cerr << "[" << src_tag << "][binance_ws] connecting to wss://" << bin_cfg.ws_host << ':'
                << bin_cfg.ws_port << bin_cfg.stream_path << "\n";
    } else {
      std::cerr << "[" << src_tag << "][spot] Polymarket RTDS Chainlink btc/usd for S + vol (no Binance WS)\n";
    }
    if (spot_latency_monitor) {
      std::cerr << "[" << src_tag << "] spot_latency_monitor=1 report_sec=" << spot_latency_report_sec
                << " (LL_SPOT_LATENCY_MONITOR / --spot-latency-monitor)\n";
    }
  }

  std::mutex print_mu;
  std::mutex paper_mu;

  // Single-thread consumer queue: callbacks only enqueue, strategy runs in one thread.
  struct Event {
    enum class Type { BinBook, BinTrade, ChainlinkSpot, PolyQuote, Rollover, Stop } type;
    ll::core::BookTickerTick book{};
    ll::core::TradeTick trade{};
    std::int64_t chainlink_ts_ms{0};
    double chainlink_px{0.0};
    ll::polymarket::PolymarketWsQuote quote{};
    std::int64_t rollover_epoch{-1};
    std::string rollover_slug;
    // Snapshot of OLD slug quotes at rollover time (for FORCE_SELL pricing).
    double rollover_up_bid{0.0};
    double rollover_up_ask{0.0};
    double rollover_down_bid{0.0};
    double rollover_down_ask{0.0};
    double rollover_price_to_beat{0.0};
  };
  std::mutex ev_mu;
  std::condition_variable ev_cv;
  std::queue<Event> ev_q;
  std::atomic<std::uint64_t> ev_q_push_depth_max_window{0};

  auto push_event = [&](Event&& e) {
    std::uint64_t depth_after = 0;
    {
      std::lock_guard<std::mutex> lk(ev_mu);
      ev_q.push(std::move(e));
      depth_after = static_cast<std::uint64_t>(ev_q.size());
    }
    std::uint64_t prev = ev_q_push_depth_max_window.load(std::memory_order_relaxed);
    while (depth_after > prev &&
           !ev_q_push_depth_max_window.compare_exchange_weak(prev, depth_after, std::memory_order_relaxed)) {
    }
    ev_cv.notify_one();
  };

  struct PaperState {
    bool have_pos{false};
    std::string side; // "Up"/"Down"
    std::string token_id;
    std::string slug;
    double qty{0.0};  // fractional shares allowed
    double cash{0.0};
    bool pending{false};
    std::string pending_action; // BUY/SELL
    std::string pending_side;
    std::int64_t due_ns{0};
    bool risk_stop{false};
    std::int64_t cooldown_until_ns{0};
  } paper;
  paper.cash = initial_cash;
  constexpr std::int64_t kCooldownNs = 10'000'000'000LL;  // 10 seconds
  bool trading_enabled = false;  // skip the very first slug; enable after first rollover

  auto entry_elapsed_ok = [&](std::int64_t active_epoch_sec, std::int64_t entry_clock_wall_ms) -> bool {
    if (!use_entry_elapsed_window) {
      return true;
    }
    if (active_epoch_sec < 0) {
      return false;
    }
    const std::int64_t bucket_start_ms = active_epoch_sec * 1000;
    const double elapsed_sec =
        (static_cast<double>(entry_clock_wall_ms - bucket_start_ms)) / 1000.0;
    return elapsed_sec >= entry_elapsed_min_sec && elapsed_sec <= entry_elapsed_max_sec;
  };

  auto schedule = [&](double p_up, double p_dn, double up_ask, double up_mid, double dn_ask, double dn_mid) {
    std::lock_guard<std::mutex> lk(paper_mu);
    const auto now_ns = ll::core::steady_ns();
    const std::int64_t lat_ns = static_cast<std::int64_t>(lat_ms) * 1'000'000LL;
    if (paper.pending) return;
    if (paper.risk_stop) return;

    if (paper.have_pos) {
      std::lock_guard<std::mutex> hk(hot.mu);
      hot.edge_up_ok_since_mono_ns = -1;
      hot.edge_dn_ok_since_mono_ns = -1;
    }

    if (!paper.have_pos) {
      if (!trading_enabled) {
        std::lock_guard<std::mutex> hk(hot.mu);
        hot.edge_up_ok_since_mono_ns = -1;
        hot.edge_dn_ok_since_mono_ns = -1;
        return;
      }
      const double e_up = p_up - up_ask;
      const double e_dn = p_dn - dn_ask;
      auto buy_ask_ok = [&](double ask) -> bool {
        if (!std::isfinite(ask) || ask <= 0.0) return false;
        if (use_buy_ask_range) return ask >= buy_ask_min && ask <= buy_ask_max;
        return ask >= 0.02;
      };
      const bool edge_up_raw = (e_up >= entry) && buy_ask_ok(up_ask);
      const bool edge_dn_raw = (e_dn >= entry) && buy_ask_ok(dn_ask);

      bool up_ok = false;
      bool dn_ok = false;
      {
        std::lock_guard<std::mutex> hk(hot.mu);
        if (edge_up_raw) {
          if (hot.edge_up_ok_since_mono_ns < 0) hot.edge_up_ok_since_mono_ns = now_ns;
        } else {
          hot.edge_up_ok_since_mono_ns = -1;
        }
        if (edge_dn_raw) {
          if (hot.edge_dn_ok_since_mono_ns < 0) hot.edge_dn_ok_since_mono_ns = now_ns;
        } else {
          hot.edge_dn_ok_since_mono_ns = -1;
        }
        if (edge_up_raw && (edge_persist_ns <= 0LL || (now_ns - hot.edge_up_ok_since_mono_ns) >= edge_persist_ns)) {
          up_ok = true;
        }
        if (edge_dn_raw && (edge_persist_ns <= 0LL || (now_ns - hot.edge_dn_ok_since_mono_ns) >= edge_persist_ns)) {
          dn_ok = true;
        }
      }
      if (!up_ok && !dn_ok) return;
      std::int64_t ae = -1;
      std::int64_t ecw = 0;
      {
        std::lock_guard<std::mutex> hk(hot.mu);
        ae = hot.active_epoch;
        ecw = hot.bin_entry_clock_wall_ms;
      }
      if (!entry_elapsed_ok(ae, ecw)) return;
      paper.pending = true;
      paper.pending_action = "BUY";
      if (up_ok && dn_ok) {
        paper.pending_side = (e_up >= e_dn) ? "Up" : "Down";
      } else if (up_ok) {
        paper.pending_side = "Up";
      } else {
        paper.pending_side = "Down";
      }
      paper.due_ns = now_ns + lat_ns;
      return;
    }

    // backtest.ipynb: Up -> up_mid >= theo_up; Down -> dn_mid >= theo_dn (optional slack via --close).
    if (paper.side == "Up") {
      if (up_mid < p_up - close_eps) return;
    } else {
      if (dn_mid < p_dn - close_eps) return;
    }
    paper.pending = true;
    paper.pending_action = "SELL";
    paper.pending_side = paper.side;
    paper.due_ns = now_ns + lat_ns;
  };

  auto execute_due = [&]() {
    std::lock_guard<std::mutex> lk(paper_mu);
    if (!paper.pending) return;
    const auto now_ns = ll::core::steady_ns();
    if (now_ns < paper.due_ns) return;

    std::string slug;
    std::int64_t active_epoch = -1;
    std::int64_t bin_wall_ms = 0;
    std::int64_t bin_entry_clock_wall_ms = 0;
    double S = 0.0, K = 0.0;
    bool have_sigma = false;
    double sigma_bucket = 0.0;
    double up_bid = 0, up_ask = 0, dn_bid = 0, dn_ask = 0;
    {
      std::lock_guard<std::mutex> lk(hot.mu);
      if (!hot.have_bin || !hot.have_K || !hot.have_up || !hot.have_down || hot.active_epoch < 0) {
        return;
      }
      slug = hot.slug;
      active_epoch = hot.active_epoch;
      bin_wall_ms = hot.bin_wall_ms;
      bin_entry_clock_wall_ms = hot.bin_entry_clock_wall_ms;
      S = hot.bin_mid;
      K = hot.K;
      have_sigma = hot.have_sigma;
      sigma_bucket = hot.sigma_bucket;
      up_bid = hot.up_bid;
      up_ask = hot.up_ask;
      dn_bid = hot.down_bid;
      dn_ask = hot.down_ask;
    }
    const std::int64_t exp_wall_ms = (active_epoch + 300) * 1000;
    const double rem_s = std::max(0.001, (exp_wall_ms - bin_wall_ms) / 1000.0);
    const double T_years = rem_s / (365.0 * 24.0 * 3600.0);
    const double sigma_use = have_sigma ? sigma_bucket : sigma_fallback;
    const double p_up = digital_call_prob(S, K, T_years, sigma_use, r);
    const double p_dn = digital_call_prob(K, S, T_years, sigma_use, r);  // placeholder; overwritten below
    const double d2 = (std::log(S / K) + (r - 0.5 * sigma_use * sigma_use) * T_years) /
                      (sigma_use * std::sqrt(T_years));
    const double disc = std::exp(-r * T_years);
    const double theo_up = clamp01(disc * normal_cdf(d2));
    const double theo_dn = clamp01(disc * normal_cdf(-d2));

    if (paper.pending_action == "BUY") {
      if (paper.have_pos) {
        paper.pending = false;
        return;
      }
      const bool want_up = (paper.pending_side == "Up");
      const double ask = want_up ? up_ask : dn_ask;
      const double mid = want_up ? 0.5 * (up_bid + up_ask) : 0.5 * (dn_bid + dn_ask);
      const double theo = want_up ? theo_up : theo_dn;
      const double edge = theo - ask;
      if (edge < entry) {
        paper.pending = false;
        return;
      }
      if (!entry_elapsed_ok(active_epoch, bin_entry_clock_wall_ms)) {
        paper.pending = false;
        return;
      }
      if (use_buy_ask_range) {
        if (!std::isfinite(ask) || ask <= 0.0 || ask < buy_ask_min || ask > buy_ask_max) {
          paper.pending = false;
          return;
        }
      } else if (ask < 0.02) {
        paper.pending = false;
        return;
      }
      // Polymarket has a practical minimum order size; enforce min $1 notional per entry when using risk_frac.
      const double spend_target =
          (fixed_spend_usd > 0.0) ? fixed_spend_usd : std::max(paper.cash * risk_frac, 1.0);
      // With fractional shares allowed, target notional spend is the key constraint.
      // Also ensure we can pay fee: spend + fee(spend) <= cash.
      const double fee_mult = fee_rate * ask * (1.0 - ask);
      const double max_spend_affordable = paper.cash / (1.0 + fee_mult);
      const double spend = std::min(spend_target, max_spend_affordable);
      if (spend < 1.0) {
        paper.pending = false;
        return;
      }
      const double qty = spend / ask;
      const double cost = spend;
      const double fee = poly_taker_fee(cost, ask, fee_rate);
      const double proj_eq = paper.cash - cost - fee + qty * mid;
      if (std::isfinite(max_loss_usd) && initial_cash - proj_eq > max_loss_usd + 1e-9) {
        paper.risk_stop = true;
        paper.pending = false;
        {
          ll::io::SyncCerrLock _;
          std::cerr << log_pfx << " risk: max-loss cap (" << max_loss_usd << " USD) would be exceeded; "
                       "blocking BUY\n";
        }
        return;
      }
      std::int64_t daemon_submit_latency_ns = -1;
#ifdef LL_ENABLE_LIVE_TRADER
      if (live_execution) {
        ll::execution::OrderIntent oi;
        oi.market_token_id = want_up ? feed_up.token_id() : feed_down.token_id();
        oi.side = "BUY";
        oi.market_order = true;
        oi.qty = spend;
        oi.mono_ns = now_ns;
        std::string errmsg;
        std::string oid;
        if (!live_ex.submit(oi, &errmsg, &oid, &daemon_submit_latency_ns)) {
          {
            ll::io::SyncCerrLock _;
            std::cerr << "[live] BUY submit failed: " << errmsg;
            if (daemon_submit_latency_ns >= 0) {
              std::cerr << " daemon_submit_latency_ms="
                        << (static_cast<double>(daemon_submit_latency_ns) / 1e6);
            }
            std::cerr << "\n";
          }
          if (trades_writer.ok()) {
            nlohmann::json row;
            row["schema_version"] = 1;
            row["source"] = src_tag;
            row["event_type"] = "error";
            row["action"] = "BUY";
            row["slug"] = slug;
            row["side"] = paper.pending_side;
            row["token_id"] = oi.market_token_id;
            row["error"] = errmsg;
            row["local_ts_wall_ms"] = ll::core::system_ms();
            row["local_ts_mono_ns"] = now_ns;
            row["theo"] = theo;
            row["ask"] = ask;
            row["edge"] = edge;
            row["qty"] = qty;
            row["spend"] = spend;
            if (daemon_submit_latency_ns >= 0) {
              row["daemon_submit_latency_ns"] = daemon_submit_latency_ns;
              row["daemon_submit_latency_ms"] = static_cast<double>(daemon_submit_latency_ns) / 1e6;
            }
            trades_writer.append(row);
          }
          paper.cooldown_until_ns = ll::core::steady_ns() + kCooldownNs;
          paper.pending = false;
          return;
        }
      }
#endif
      paper.cash -= (cost + fee);
      paper.have_pos = true;
      paper.side = paper.pending_side;
      paper.slug = slug;
      paper.token_id = want_up ? feed_up.token_id() : feed_down.token_id();
      paper.qty = qty;
      {
        double eq = paper.cash;
        if (paper.have_pos) {
          eq += paper.qty * mid;
        }
        if (std::isfinite(max_loss_usd) && initial_cash - eq >= max_loss_usd - 1e-9) {
          paper.risk_stop = true;
        }
      }
      {
        nlohmann::json row;
        row["schema_version"] = 1;
        row["source"] = src_tag;
        row["event_type"] = "trade";
        row["action"] = "BUY";
        row["slug"] = paper.slug;
        row["side"] = paper.side;
        row["token_id"] = paper.token_id;
        row["local_ts_mono_ns"] = now_ns;
        row["local_ts_wall_ms"] = ll::core::system_ms();
        row["S"] = S;
        row["K"] = K;
        row["T_s"] = rem_s;
        row["sigma"] = sigma_use;
        row["theo"] = theo;
        row["bid"] = (want_up ? up_bid : dn_bid);
        row["ask"] = ask;
        row["mid"] = mid;
        row["edge"] = edge;
        row["qty"] = qty;
        row["cost"] = cost;
        row["fee"] = fee;
        row["spend_target"] = spend_target;
        row["spend"] = cost;
        row["cash_before"] = (paper.cash + cost + fee);
        row["cash_after"] = paper.cash;
        if (live_execution && daemon_submit_latency_ns >= 0) {
          row["daemon_submit_latency_ns"] = daemon_submit_latency_ns;
          row["daemon_submit_latency_ms"] = static_cast<double>(daemon_submit_latency_ns) / 1e6;
        }
        trades_writer.append(row);
      }
      {
        std::lock_guard<std::mutex> pk(print_mu);
        std::cout << log_pfx << " BUY side=" << paper.side << " slug=" << paper.slug
                  << " S=" << S << " K=" << K << " T_s=" << rem_s << " sigma=" << sigma_use
                  << " theo=" << theo << " bid=" << (want_up ? up_bid : dn_bid) << " ask=" << ask
                  << " mid=" << mid << " edge=" << edge << " qty=" << qty
                  << " cost=" << cost << " fee=" << fee
                  << " spend_target=" << spend_target << " spend=" << cost
                  << " cash_before=" << (paper.cash + cost + fee) << " cash_after=" << paper.cash;
        if (live_execution && daemon_submit_latency_ns >= 0) {
          std::cout << " daemon_submit_ms=" << (static_cast<double>(daemon_submit_latency_ns) / 1e6);
        }
        std::cout << "\n";
      }
      paper.pending = false;
      return;
    }

    if (paper.pending_action == "SELL" && paper.have_pos) {
      const bool is_up = (paper.side == "Up");
      const double bid = is_up ? up_bid : dn_bid;
      const double ask = is_up ? up_ask : dn_ask;
      const double mid = 0.5 * (bid + ask);
      const double up_mid_ex = 0.5 * (up_bid + up_ask);
      const double dn_mid_ex = 0.5 * (dn_bid + dn_ask);
      const double theo = is_up ? theo_up : theo_dn;
      const double edge = theo - ask;
      // Match schedule(): mid >= theo - close_eps; re-check at execute time (backtest.ipynb exit rule).
      if (is_up) {
        if (up_mid_ex < theo_up - close_eps) {
          paper.pending = false;
          return;
        }
      } else {
        if (dn_mid_ex < theo_dn - close_eps) {
          paper.pending = false;
          return;
        }
      }
      const double strategy_qty = paper.qty;
      double sell_qty = strategy_qty;
      double clob_balance_shares = std::numeric_limits<double>::quiet_NaN();
      std::int64_t daemon_query_latency_ns = -1;
#ifdef LL_ENABLE_LIVE_TRADER
      if (live_execution) {
        std::string qerr;
        if (!live_ex.query_conditional_balance(paper.token_id, &clob_balance_shares, &qerr,
                                               &daemon_query_latency_ns)) {
          {
            ll::io::SyncCerrLock _;
            std::cerr << "[live] SELL conditional balance query failed: " << qerr;
            if (daemon_query_latency_ns >= 0) {
              std::cerr << " daemon_query_latency_ms="
                        << (static_cast<double>(daemon_query_latency_ns) / 1e6);
            }
            std::cerr << "\n";
          }
          if (trades_writer.ok()) {
            nlohmann::json row;
            row["schema_version"] = 1;
            row["source"] = src_tag;
            row["event_type"] = "error";
            row["action"] = "SELL_BALANCE_QUERY";
            row["slug"] = paper.slug;
            row["side"] = paper.side;
            row["token_id"] = paper.token_id;
            row["error"] = qerr;
            row["local_ts_wall_ms"] = ll::core::system_ms();
            row["local_ts_mono_ns"] = now_ns;
            row["strategy_qty"] = strategy_qty;
            if (daemon_query_latency_ns >= 0) {
              row["daemon_query_latency_ns"] = daemon_query_latency_ns;
              row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_ns) / 1e6;
            }
            trades_writer.append(row);
          }
          paper.cooldown_until_ns = ll::core::steady_ns() + kCooldownNs;
          paper.pending = false;
          return;
        }
        // Live SELL size: use CLOB conditional balance only (do not cap by strategy_qty).
        sell_qty = std::max(0.0, clob_balance_shares);
      }
#endif
      if (sell_qty <= 1e-12) {
        if (live_execution) {
          {
            ll::io::SyncCerrLock _;
            std::cerr << "[live] SELL skipped: clob_balance=" << clob_balance_shares
                      << " (API reported 0 or negative sellable shares; keeping position for retry)\n";
          }
          if (trades_writer.ok()) {
            nlohmann::json row;
            row["schema_version"] = 1;
            row["source"] = src_tag;
            row["event_type"] = "error";
            row["action"] = "SELL_SKIPPED_ZERO";
            row["slug"] = paper.slug;
            row["side"] = paper.side;
            row["token_id"] = paper.token_id;
            row["error"] = "sell_qty from API balance is 0";
            row["local_ts_wall_ms"] = ll::core::system_ms();
            row["local_ts_mono_ns"] = now_ns;
            row["strategy_qty"] = strategy_qty;
            if (std::isfinite(clob_balance_shares)) {
              row["clob_balance"] = clob_balance_shares;
            }
            if (daemon_query_latency_ns >= 0) {
              row["daemon_query_latency_ns"] = daemon_query_latency_ns;
              row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_ns) / 1e6;
            }
            trades_writer.append(row);
          }
          paper.cooldown_until_ns = ll::core::steady_ns() + kCooldownNs;
        }
        paper.pending = false;
        return;
      }
      const double proceeds = sell_qty * bid;
      const double fee = poly_taker_fee(proceeds, bid, fee_rate);
      const double cash_before = paper.cash;
      std::int64_t daemon_submit_latency_ns = -1;
#ifdef LL_ENABLE_LIVE_TRADER
      if (live_execution) {
        ll::execution::OrderIntent oi;
        oi.market_token_id = paper.token_id;
        oi.side = "SELL";
        oi.market_order = true;
        oi.qty = sell_qty;
        oi.mono_ns = now_ns;
        std::string errmsg;
        std::string oid;
        if (!live_ex.submit(oi, &errmsg, &oid, &daemon_submit_latency_ns)) {
          {
            ll::io::SyncCerrLock _;
            std::cerr << "[live] SELL submit failed: " << errmsg;
            if (daemon_submit_latency_ns >= 0) {
              std::cerr << " daemon_submit_latency_ms="
                        << (static_cast<double>(daemon_submit_latency_ns) / 1e6);
            }
            std::cerr << "\n";
          }
          if (trades_writer.ok()) {
            nlohmann::json row;
            row["schema_version"] = 1;
            row["source"] = src_tag;
            row["event_type"] = "error";
            row["action"] = "SELL";
            row["slug"] = paper.slug;
            row["side"] = paper.side;
            row["token_id"] = paper.token_id;
            row["error"] = errmsg;
            row["local_ts_wall_ms"] = ll::core::system_ms();
            row["local_ts_mono_ns"] = now_ns;
            row["theo"] = theo;
            row["bid"] = bid;
            row["edge"] = edge;
            row["strategy_qty"] = strategy_qty;
            row["sell_qty"] = sell_qty;
            if (std::isfinite(clob_balance_shares)) {
              row["clob_balance"] = clob_balance_shares;
            }
            if (daemon_query_latency_ns >= 0) {
              row["daemon_query_latency_ns"] = daemon_query_latency_ns;
              row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_ns) / 1e6;
            }
            if (daemon_submit_latency_ns >= 0) {
              row["daemon_submit_latency_ns"] = daemon_submit_latency_ns;
              row["daemon_submit_latency_ms"] = static_cast<double>(daemon_submit_latency_ns) / 1e6;
            }
            trades_writer.append(row);
          }
          paper.cooldown_until_ns = ll::core::steady_ns() + kCooldownNs;
          paper.pending = false;
          return;
        }
      }
#endif
      paper.cash += (proceeds - fee);
      {
        nlohmann::json row;
        row["schema_version"] = 1;
        row["source"] = src_tag;
        row["event_type"] = "trade";
        row["action"] = "SELL";
        row["slug"] = paper.slug;
        row["side"] = paper.side;
        row["token_id"] = paper.token_id;
        row["local_ts_mono_ns"] = now_ns;
        row["local_ts_wall_ms"] = ll::core::system_ms();
        row["S"] = S;
        row["K"] = K;
        row["T_s"] = rem_s;
        row["sigma"] = sigma_use;
        row["theo"] = theo;
        row["bid"] = bid;
        row["ask"] = ask;
        row["mid"] = mid;
        row["edge"] = edge;
        row["qty"] = sell_qty;
        row["strategy_qty"] = strategy_qty;
        if (live_execution && std::isfinite(clob_balance_shares)) {
          row["clob_balance_shares"] = clob_balance_shares;
          row["qty_clamped"] = (std::abs(sell_qty - strategy_qty) > 1e-12);
        }
        row["proceeds"] = proceeds;
        row["fee"] = fee;
        row["cash_before"] = cash_before;
        row["cash_after"] = paper.cash;
        if (live_execution && daemon_query_latency_ns >= 0) {
          row["daemon_query_latency_ns"] = daemon_query_latency_ns;
          row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_ns) / 1e6;
        }
        if (live_execution && daemon_submit_latency_ns >= 0) {
          row["daemon_submit_latency_ns"] = daemon_submit_latency_ns;
          row["daemon_submit_latency_ms"] = static_cast<double>(daemon_submit_latency_ns) / 1e6;
        }
        trades_writer.append(row);
      }
      {
        std::lock_guard<std::mutex> pk(print_mu);
        std::cout << log_pfx << " SELL side=" << paper.side << " slug=" << paper.slug
                  << " S=" << S << " K=" << K << " T_s=" << rem_s << " sigma=" << sigma_use
                  << " theo=" << theo << " bid=" << bid << " ask=" << ask << " mid=" << mid
                  << " edge=" << edge << " qty=" << sell_qty << " proceeds=" << proceeds
                  << " fee=" << fee << " cash_before=" << cash_before << " cash_after=" << paper.cash;
        if (live_execution && std::isfinite(clob_balance_shares)) {
          std::cout << " strategy_qty=" << strategy_qty << " clob_balance=" << clob_balance_shares;
        }
        if (live_execution && daemon_query_latency_ns >= 0) {
          std::cout << " daemon_query_ms=" << (static_cast<double>(daemon_query_latency_ns) / 1e6);
        }
        if (live_execution && daemon_submit_latency_ns >= 0) {
          std::cout << " daemon_submit_ms=" << (static_cast<double>(daemon_submit_latency_ns) / 1e6);
        }
        std::cout << "\n";
      }
      {
        const double eq_flat = paper.cash;
        if (std::isfinite(max_loss_usd) && initial_cash - eq_flat >= max_loss_usd - 1e-9) {
          paper.risk_stop = true;
        }
      }
      paper.have_pos = false;
      paper.qty = 0.0;
      paper.side.clear();
      paper.slug.clear();
      paper.token_id.clear();
      paper.pending = false;
      return;
    }
    // Unknown state; clear pending.
    paper.pending = false;
  };

  auto on_hot_update = [&]() {
    // Called after any bin/poly update; compute theo + schedule/execute.
    std::int64_t active_epoch = -1;
    std::int64_t bin_wall_ms = 0;
    double S = 0.0, K = 0.0;
    bool have_sigma = false;
    double sigma_bucket = 0.0;
    double up_bid = 0, up_ask = 0, dn_bid = 0, dn_ask = 0;
    {
      std::lock_guard<std::mutex> lk(hot.mu);
      if (!hot.have_bin || !hot.have_K || !hot.have_up || !hot.have_down || hot.active_epoch < 0) return;
      active_epoch = hot.active_epoch;
      bin_wall_ms = hot.bin_wall_ms;
      S = hot.bin_mid;
      K = hot.K;
      have_sigma = hot.have_sigma;
      sigma_bucket = hot.sigma_bucket;
      up_bid = hot.up_bid;
      up_ask = hot.up_ask;
      dn_bid = hot.down_bid;
      dn_ask = hot.down_ask;
    }
    const std::int64_t exp_wall_ms = (active_epoch + 300) * 1000;
    const double rem_s = std::max(0.001, (exp_wall_ms - bin_wall_ms) / 1000.0);
    const double T_years = rem_s / (365.0 * 24.0 * 3600.0);
    const double sigma_use = have_sigma ? sigma_bucket : sigma_fallback;
    const double d2 = (std::log(S / K) + (r - 0.5 * sigma_use * sigma_use) * T_years) /
                      (sigma_use * std::sqrt(T_years));
    const double disc = std::exp(-r * T_years);
    const double p_up = clamp01(disc * normal_cdf(d2));
    const double p_dn = clamp01(disc * normal_cdf(-d2));
    const double up_mid = 0.5 * (up_bid + up_ask);
    const double dn_mid = 0.5 * (dn_bid + dn_ask);

    // Record theo vs market series (rate-limited).
    if (series_writer.ok()) {
      const auto now_ns = ll::core::steady_ns();
      if (last_series_mono_ns == 0 || now_ns - last_series_mono_ns >= kSeriesEveryNs) {
        last_series_mono_ns = now_ns;
        nlohmann::json row;
        row["schema_version"] = 1;
        row["source"] = src_tag;
        row["event_type"] = "series";
        row["local_ts_mono_ns"] = now_ns;
        row["local_ts_wall_ms"] = ll::core::system_ms();
        row["active_epoch"] = active_epoch;
        row["T_s"] = rem_s;
        row["S"] = S;
        row["K"] = K;
        row["sigma"] = sigma_use;
        row["theo_up"] = p_up;
        row["theo_down"] = p_dn;
        row["up"] = {{"bid", up_bid}, {"ask", up_ask}, {"mid", up_mid}};
        row["down"] = {{"bid", dn_bid}, {"ask", dn_ask}, {"mid", dn_mid}};
        {
          std::lock_guard<std::mutex> lk(paper_mu);
          row["cash"] = paper.cash;
          row["have_pos"] = paper.have_pos;
          row["pos_side"] = paper.side;
          row["pos_qty"] = paper.qty;
          row["trading_enabled"] = trading_enabled;
        }
        series_writer.append(row);
      }
    }

    bool enabled = false;
    {
      std::lock_guard<std::mutex> lk(paper_mu);
      enabled = trading_enabled;
    }
    if (!enabled) return;
    schedule(p_up, p_dn, up_ask, up_mid, dn_ask, dn_mid);
    execute_due();
  };

  std::thread consumer_thr([&] {
    std::int64_t spot_lat_report_last_mono_ns = 0;
    std::vector<std::int64_t> spot_lat_total_ns;
    std::vector<std::int64_t> spot_lat_sigma_ns;
    std::vector<std::int64_t> spot_lat_q_rem;
    const std::int64_t spot_lat_report_period_ns =
        static_cast<std::int64_t>(spot_latency_report_sec * 1e9);
    spot_lat_total_ns.reserve(4096);
    spot_lat_sigma_ns.reserve(4096);
    spot_lat_q_rem.reserve(4096);

    auto maybe_report_spot_latency = [&]() {
      if (!spot_latency_monitor) return;
      const auto now = ll::core::steady_ns();
      if (spot_lat_report_last_mono_ns == 0) {
        spot_lat_report_last_mono_ns = now;
        return;
      }
      if (now - spot_lat_report_last_mono_ns < spot_lat_report_period_ns) return;
      spot_lat_report_last_mono_ns = now;

      const std::uint64_t push_q_max =
          ev_q_push_depth_max_window.exchange(0, std::memory_order_relaxed);

      const std::size_t n = spot_lat_total_ns.size();
      if (n == 0) {
        ll::io::SyncCerrLock _;
        std::cerr << log_pfx << " spot_latency summary window_sec=" << spot_latency_report_sec
                  << " samples=0 push_q_max_window=" << push_q_max << "\n";
        return;
      }

      std::vector<std::int64_t> tot = spot_lat_total_ns;
      std::vector<std::int64_t> sig = spot_lat_sigma_ns;
      std::vector<std::int64_t> qrem = spot_lat_q_rem;
      spot_lat_total_ns.clear();
      spot_lat_sigma_ns.clear();
      spot_lat_q_rem.clear();

      const double tot_p50 = percentile_sorted_copy(tot, 0.50) / 1000.0;
      const double tot_p95 = percentile_sorted_copy(tot, 0.95) / 1000.0;
      const double tot_p99 = percentile_sorted_copy(tot, 0.99) / 1000.0;
      const double tot_max =
          static_cast<double>(*std::max_element(tot.begin(), tot.end())) / 1000.0;

      const double sig_p50 = percentile_sorted_copy(sig, 0.50) / 1000.0;
      const double sig_p95 = percentile_sorted_copy(sig, 0.95) / 1000.0;
      const double sig_p99 = percentile_sorted_copy(sig, 0.99) / 1000.0;
      const double sig_max =
          static_cast<double>(*std::max_element(sig.begin(), sig.end())) / 1000.0;

      const double qrem_p50 = percentile_sorted_copy(qrem, 0.50);
      const double qrem_p99 = percentile_sorted_copy(qrem, 0.99);
      const double qrem_max =
          static_cast<double>(*std::max_element(qrem.begin(), qrem.end()));

      ll::io::SyncCerrLock _;
      std::cerr << log_pfx << " spot_latency summary window_sec=" << spot_latency_report_sec
                << " samples=" << n << " push_q_max_window=" << push_q_max
                << " q_rem_p50=" << qrem_p50 << " q_rem_p99=" << qrem_p99 << " q_rem_max=" << qrem_max
                << " spot_total_us p50=" << tot_p50 << " p95=" << tot_p95 << " p99=" << tot_p99
                << " max=" << tot_max << " | sigma_path_us p50=" << sig_p50 << " p95=" << sig_p95
                << " p99=" << sig_p99 << " max=" << sig_max << "\n";
    };

    for (;;) {
      Event ev;
      std::size_t q_rem = 0;
      {
        std::unique_lock<std::mutex> lk(ev_mu);
        ev_cv.wait(lk, [&] { return g_stop.load(std::memory_order_relaxed) || !ev_q.empty(); });
        if (ev_q.empty() && g_stop.load(std::memory_order_relaxed)) {
          return;
        }
        ev = std::move(ev_q.front());
        ev_q.pop();
        q_rem = ev_q.size();
      }

      maybe_report_spot_latency();

      if (ev.type == Event::Type::Stop) {
        return;
      }

      if (ev.type == Event::Type::Rollover) {
        // Optional: FORCE_SELL any open position at rollover using current best bid.
        {
          std::lock_guard<std::mutex> lk(paper_mu);
          if (paper.have_pos && disable_rollover_force_sell) {
            {
              ll::io::SyncCerrLock _;
              std::cerr << log_pfx << " rollover: FORCE_SELL disabled; carrying position for manual settlement"
                        << " side=" << paper.side << " qty=" << paper.qty << " token_id=" << paper.token_id
                        << " slug=" << paper.slug << "\n";
            }
            paper.have_pos = false;
            paper.pending = false;
            paper.qty = 0.0;
            paper.side.clear();
            paper.slug.clear();
            paper.token_id.clear();
          } else if (paper.have_pos) {
            const double bid = (paper.side == "Up") ? ev.rollover_up_bid : ev.rollover_down_bid;
            const double strategy_qty_fs = paper.qty;
            double sell_qty_fs = strategy_qty_fs;
            double clob_balance_fs = std::numeric_limits<double>::quiet_NaN();
            std::int64_t daemon_query_latency_fs_ns = -1;
            bool balance_query_failed = false;
#ifdef LL_ENABLE_LIVE_TRADER
            if (live_execution) {
              std::string qerr;
              if (!live_ex.query_conditional_balance(paper.token_id, &clob_balance_fs, &qerr,
                                                     &daemon_query_latency_fs_ns)) {
                {
                  ll::io::SyncCerrLock _;
                  std::cerr << "[live] FORCE_SELL conditional balance query failed: " << qerr;
                  if (daemon_query_latency_fs_ns >= 0) {
                    std::cerr << " daemon_query_latency_ms="
                              << (static_cast<double>(daemon_query_latency_fs_ns) / 1e6);
                  }
                  std::cerr << "\n";
                }
                balance_query_failed = true;
                if (trades_writer.ok()) {
                  const auto wall_ms = ll::core::system_ms();
                  const auto mono_ns = ll::core::steady_ns();
                  nlohmann::json row;
                  row["schema_version"] = 1;
                  row["source"] = src_tag;
                  row["event_type"] = "error";
                  row["action"] = "FORCE_SELL_BALANCE_QUERY";
                  row["slug"] = paper.slug;
                  row["side"] = paper.side;
                  row["token_id"] = paper.token_id;
                  row["error"] = qerr;
                  row["local_ts_wall_ms"] = wall_ms;
                  row["local_ts_mono_ns"] = mono_ns;
                  row["strategy_qty"] = strategy_qty_fs;
                  if (daemon_query_latency_fs_ns >= 0) {
                    row["daemon_query_latency_ns"] = daemon_query_latency_fs_ns;
                    row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_fs_ns) / 1e6;
                  }
                  trades_writer.append(row);
                }
              } else {
                // FORCE_SELL size: CLOB balance only (do not cap by strategy_qty).
                sell_qty_fs = std::max(0.0, clob_balance_fs);
              }
            }
#endif
            if (balance_query_failed) {
              // Keep open position; rollover slug/K reset still happens below.
            } else if (sell_qty_fs <= 1e-12) {
              if (live_execution) {
                {
                  ll::io::SyncCerrLock _;
                  std::cerr << "[live] FORCE_SELL skipped: clob_balance=" << clob_balance_fs
                            << " (API reported 0 sellable shares)\n";
                }
                if (trades_writer.ok()) {
                  nlohmann::json row;
                  row["schema_version"] = 1;
                  row["source"] = src_tag;
                  row["event_type"] = "error";
                  row["action"] = "FORCE_SELL_SKIPPED_ZERO";
                  row["slug"] = paper.slug;
                  row["side"] = paper.side;
                  row["token_id"] = paper.token_id;
                  row["error"] = "sell_qty from API balance is 0 at rollover FORCE_SELL";
                  row["local_ts_wall_ms"] = ll::core::system_ms();
                  row["local_ts_mono_ns"] = ll::core::steady_ns();
                  row["strategy_qty"] = strategy_qty_fs;
                  if (std::isfinite(clob_balance_fs)) {
                    row["clob_balance"] = clob_balance_fs;
                  }
                  if (daemon_query_latency_fs_ns >= 0) {
                    row["daemon_query_latency_ns"] = daemon_query_latency_fs_ns;
                    row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_fs_ns) / 1e6;
                  }
                  trades_writer.append(row);
                }
              }
              paper.have_pos = false;
              paper.pending = false;
              paper.qty = 0.0;
              paper.side.clear();
              paper.slug.clear();
              paper.token_id.clear();
            } else {
              const double proceeds = sell_qty_fs * bid;
              const double fee = poly_taker_fee(proceeds, bid, fee_rate);
              const double cash_before = paper.cash;
              bool live_fs_ok = true;
              std::int64_t daemon_submit_latency_ns = -1;
#ifdef LL_ENABLE_LIVE_TRADER
              if (live_execution) {
                ll::execution::OrderIntent oi;
                oi.market_token_id = paper.token_id;
                oi.side = "SELL";
                oi.market_order = true;
                oi.qty = sell_qty_fs;
                oi.mono_ns = ll::core::steady_ns();
                std::string errmsg;
                std::string oid;
                if (!live_ex.submit(oi, &errmsg, &oid, &daemon_submit_latency_ns)) {
                  {
                    ll::io::SyncCerrLock _;
                    std::cerr << "[live] FORCE_SELL submit failed: " << errmsg;
                    if (daemon_submit_latency_ns >= 0) {
                      std::cerr << " daemon_submit_latency_ms="
                                << (static_cast<double>(daemon_submit_latency_ns) / 1e6);
                    }
                    std::cerr << "\n";
                  }
                  live_fs_ok = false;
                  if (trades_writer.ok()) {
                    nlohmann::json erow;
                    erow["schema_version"] = 1;
                    erow["source"] = src_tag;
                    erow["event_type"] = "error";
                    erow["action"] = "FORCE_SELL";
                    erow["slug"] = paper.slug;
                    erow["side"] = paper.side;
                    erow["token_id"] = paper.token_id;
                    erow["error"] = errmsg;
                    erow["local_ts_wall_ms"] = ll::core::system_ms();
                    erow["local_ts_mono_ns"] = ll::core::steady_ns();
                    erow["bid"] = bid;
                    erow["strategy_qty"] = strategy_qty_fs;
                    erow["sell_qty"] = sell_qty_fs;
                    if (std::isfinite(clob_balance_fs)) {
                      erow["clob_balance"] = clob_balance_fs;
                    }
                    if (daemon_query_latency_fs_ns >= 0) {
                      erow["daemon_query_latency_ns"] = daemon_query_latency_fs_ns;
                      erow["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_fs_ns) / 1e6;
                    }
                    if (daemon_submit_latency_ns >= 0) {
                      erow["daemon_submit_latency_ns"] = daemon_submit_latency_ns;
                      erow["daemon_submit_latency_ms"] = static_cast<double>(daemon_submit_latency_ns) / 1e6;
                    }
                    trades_writer.append(erow);
                  }
                }
              }
#endif
              if (!live_execution || live_fs_ok) {
                paper.cash += (proceeds - fee);
                nlohmann::json row;
                row["schema_version"] = 1;
                row["source"] = src_tag;
                row["event_type"] = "trade";
                row["action"] = "FORCE_SELL";
                row["slug"] = paper.slug;
                row["side"] = paper.side;
                row["token_id"] = paper.token_id;
                row["local_ts_mono_ns"] = ll::core::steady_ns();
                row["local_ts_wall_ms"] = ll::core::system_ms();
                row["bid"] = bid;
                row["qty"] = sell_qty_fs;
                row["strategy_qty"] = strategy_qty_fs;
                if (live_execution && std::isfinite(clob_balance_fs)) {
                  row["clob_balance_shares"] = clob_balance_fs;
                  row["qty_clamped"] = (std::abs(sell_qty_fs - strategy_qty_fs) > 1e-12);
                }
                row["proceeds"] = proceeds;
                row["fee"] = fee;
                row["cash_before"] = cash_before;
                row["cash_after"] = paper.cash;
                row["note"] = "rollover";
                if (live_execution && daemon_query_latency_fs_ns >= 0) {
                  row["daemon_query_latency_ns"] = daemon_query_latency_fs_ns;
                  row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_fs_ns) / 1e6;
                }
                if (live_execution && daemon_submit_latency_ns >= 0) {
                  row["daemon_submit_latency_ns"] = daemon_submit_latency_ns;
                  row["daemon_submit_latency_ms"] = static_cast<double>(daemon_submit_latency_ns) / 1e6;
                }
                trades_writer.append(row);
                std::lock_guard<std::mutex> pk(print_mu);
                std::cout << log_pfx << " FORCE_SELL side=" << paper.side << " slug=" << paper.slug << " px=" << bid
                          << " qty=" << sell_qty_fs << " proceeds=" << proceeds << " fee=" << fee
                          << " cash_before=" << cash_before << " cash_after=" << paper.cash;
                if (live_execution && std::isfinite(clob_balance_fs)) {
                  std::cout << " strategy_qty=" << strategy_qty_fs << " clob_balance=" << clob_balance_fs;
                }
                if (live_execution && daemon_query_latency_fs_ns >= 0) {
                  std::cout << " daemon_query_ms=" << (static_cast<double>(daemon_query_latency_fs_ns) / 1e6);
                }
                if (live_execution && daemon_submit_latency_ns >= 0) {
                  std::cout << " daemon_submit_ms=" << (static_cast<double>(daemon_submit_latency_ns) / 1e6);
                }
                std::cout << "\n";
                paper.have_pos = false;
                paper.pending = false;
                paper.qty = 0.0;
                paper.side.clear();
                paper.slug.clear();
                paper.token_id.clear();
              }
            }
          }
        }
        {
          nlohmann::json row;
          row["schema_version"] = 1;
          row["source"] = src_tag;
          row["event_type"] = "rollover";
          row["new_slug"] = ev.rollover_slug;
          row["new_epoch"] = ev.rollover_epoch;
          if (std::isfinite(ev.rollover_price_to_beat)) {
            row["price_to_beat"] = ev.rollover_price_to_beat;
          } else {
            row["price_to_beat"] = nullptr;
          }
          row["local_ts_wall_ms"] = ll::core::system_ms();
          row["disable_force_sell"] = disable_rollover_force_sell;
          {
            std::lock_guard<std::mutex> lk(paper_mu);
            row["cash"] = paper.cash;
            row["have_pos"] = paper.have_pos;
            if (paper.have_pos) {
              row["pos_side"] = paper.side;
              row["pos_qty"] = paper.qty;
              row["pos_slug"] = paper.slug;
              row["pos_token_id"] = paper.token_id;
            }
          }
          trades_writer.append(row);
        }
        {
          std::lock_guard<std::mutex> hk(hot.mu);
          hot.slug = ev.rollover_slug;
          hot.active_epoch = ev.rollover_epoch;
          if (std::isfinite(ev.rollover_price_to_beat) && ev.rollover_price_to_beat > 0.0) {
            hot.have_K = true;
            hot.K = ev.rollover_price_to_beat;
          } else {
            hot.have_K = false;
          }
          hot.have_sigma = false;
          hot.sigma_bucket = 0.0;
          hot.edge_up_ok_since_mono_ns = -1;
          hot.edge_dn_ok_since_mono_ns = -1;
        }
        {
          std::lock_guard<std::mutex> lk(paper_mu);
          trading_enabled = true;
        }
        continue;
      }

      if (ev.type == Event::Type::ChainlinkSpot || ev.type == Event::Type::BinBook ||
          ev.type == Event::Type::BinTrade) {
        const std::int64_t t_spot_begin =
            spot_latency_monitor ? ll::core::steady_ns() : std::int64_t{0};
        if (ev.type == Event::Type::ChainlinkSpot && chainlink_ticks_writer &&
            chainlink_ticks_writer->ok()) {
          const auto n = chainlink_tick_seq.fetch_add(1, std::memory_order_relaxed) + 1;
          nlohmann::json row;
          row["schema_version"] = 1;
          row["source"] = "polymarket_rtds_chainlink";
          row["symbol"] = "BTC/USD";
          row["payload_ts_ms"] = ev.chainlink_ts_ms;
          row["local_ts_mono_ns"] = ll::core::steady_ns();
          row["local_ts_wall_ms"] = ll::core::system_ms();
          row["seq"] = n;
          row["event_type"] = "chainlink";
          row["payload"] = {{"price", ev.chainlink_px}};
          chainlink_ticks_writer->append(row);
        }
        const double mid = (ev.type == Event::Type::ChainlinkSpot)
                               ? ev.chainlink_px
                               : (ev.type == Event::Type::BinBook)
                                     ? 0.5 * (ev.book.bid_price + ev.book.ask_price)
                                     : ev.trade.price;
        const std::int64_t wall_ms = (ev.type == Event::Type::ChainlinkSpot)
                                         ? ev.chainlink_ts_ms
                                         : (ev.type == Event::Type::BinBook)
                                               ? ev.book.local_wall_ms
                                               : ev.trade.local_wall_ms;
        const std::int64_t entry_clock_wall_ms =
            (ev.type == Event::Type::ChainlinkSpot) ? ll::core::system_ms() : wall_ms;
        const std::int64_t wall_s = wall_ms / 1000;

        const std::int64_t t_sigma_begin =
            spot_latency_monitor ? ll::core::steady_ns() : std::int64_t{0};
        bool need_sigma = false;
        bool have_full_hour = false;
        std::vector<double> resampled_mids;

        {
          std::lock_guard<std::mutex> lk(hot.mu);
          hot.have_bin = true;
          hot.bin_mid = mid;
          hot.bin_wall_ms = wall_ms;
          hot.bin_entry_clock_wall_ms = entry_clock_wall_ms;
          hot.bin_hist.emplace_back(wall_ms, mid);
          while (!hot.bin_hist.empty() && hot.bin_hist.front().first < wall_ms - 3600 * 1000) {
            hot.bin_hist.pop_front();
          }
          if (hot.active_epoch >= 0 && !hot.have_sigma && wall_s >= hot.active_epoch) {
            need_sigma = true;
            have_full_hour =
                !hot.bin_hist.empty() && (wall_ms - hot.bin_hist.front().first >= 3600 * 1000);
            if (have_full_hour) {
              resampled_mids = resample_mids_from_hist(hot.bin_hist, wall_ms, sigma_step_ms);
            }
          }
        }

        if (need_sigma && have_full_hour && !resampled_mids.empty()) {
          double final_sig = std::numeric_limits<double>::quiet_NaN();
#ifdef LL_ENABLE_GARCH_DAEMON
          if (sigma_try_garch) {
            double garch_sig = 0.0;
            std::string gerr;
            std::int64_t glat = -1;
            if (live_ex.query_garch_sigma(resampled_mids, sigma_step_ms, &garch_sig, &gerr, &glat)) {
              if (std::isfinite(garch_sig) && garch_sig >= sigma_min && garch_sig <= sigma_max) {
                final_sig = garch_sig;
                {
                  ll::io::SyncCerrLock _;
                  std::cerr << log_pfx << " GARCH sigma=" << garch_sig
                            << " n_mids=" << resampled_mids.size();
                  if (glat >= 0) std::cerr << " latency_ms=" << (static_cast<double>(glat) / 1e6);
                  std::cerr << "\n";
                }
              } else {
                {
                  ll::io::SyncCerrLock _;
                  std::cerr << log_pfx << " GARCH sigma out of range: " << garch_sig
                            << ", falling back to realized vol\n";
                }
              }
            } else {
              {
                ll::io::SyncCerrLock _;
                std::cerr << log_pfx << " GARCH failed: " << gerr << ", falling back to realized vol\n";
              }
            }
          }
#endif
          if (!std::isfinite(final_sig) && resampled_mids.size() >= 10) {
            std::vector<double> rets;
            rets.reserve(resampled_mids.size() - 1);
            for (std::size_t ri = 1; ri < resampled_mids.size(); ++ri) {
              const double lr = std::log(resampled_mids[ri] / resampled_mids[ri - 1]);
              if (std::isfinite(lr)) rets.push_back(lr);
            }
            if (rets.size() >= 10) {
              double mean = 0.0;
              for (double x : rets) mean += x;
              mean /= static_cast<double>(rets.size());
              double var = 0.0;
              for (double x : rets) {
                double d = x - mean;
                var += d * d;
              }
              var /= static_cast<double>(rets.size() - 1);
              const double steps_per_year =
                  (365.0 * 24.0 * 3600.0 * 1000.0) / static_cast<double>(sigma_step_ms);
              final_sig = std::sqrt(var * steps_per_year);
            }
          }

          {
            std::lock_guard<std::mutex> lk(hot.mu);
            if (std::isfinite(final_sig) && final_sig >= sigma_min && final_sig <= sigma_max) {
              hot.have_sigma = true;
              hot.sigma_bucket = final_sig;
            } else {
              hot.have_sigma = false;
              hot.sigma_bucket = 0.0;
            }
          }
        } else if (need_sigma) {
          std::lock_guard<std::mutex> lk(hot.mu);
          hot.have_sigma = false;
          hot.sigma_bucket = 0.0;
        }

        // Rolling realized σ every spot tick (data/backtest.ipynb), after optional first-time GARCH path.
        {
          bool in_bucket = false;
          bool rv_full_hour = false;
          std::vector<double> rv_mids;
          {
            std::lock_guard<std::mutex> lk(hot.mu);
            in_bucket = hot.active_epoch >= 0 && wall_s >= hot.active_epoch;
            if (in_bucket) {
              rv_full_hour =
                  !hot.bin_hist.empty() && (wall_ms - hot.bin_hist.front().first >= 3600 * 1000);
              if (rv_full_hour) {
                rv_mids = resample_mids_from_hist(hot.bin_hist, wall_ms, sigma_step_ms);
              }
            }
          }
          if (in_bucket && rv_full_hour && rv_mids.size() >= 10) {
            std::vector<double> rets;
            rets.reserve(rv_mids.size() - 1);
            for (std::size_t ri = 1; ri < rv_mids.size(); ++ri) {
              const double lr = std::log(rv_mids[ri] / rv_mids[ri - 1]);
              if (std::isfinite(lr)) rets.push_back(lr);
            }
            if (rets.size() >= 10) {
              double mean = 0.0;
              for (double x : rets) mean += x;
              mean /= static_cast<double>(rets.size());
              double var = 0.0;
              for (double x : rets) {
                double d = x - mean;
                var += d * d;
              }
              var /= static_cast<double>(rets.size() - 1);
              const double steps_per_year =
                  (365.0 * 24.0 * 3600.0 * 1000.0) / static_cast<double>(sigma_step_ms);
              const double rv_sig = std::sqrt(var * steps_per_year);
              if (std::isfinite(rv_sig) && rv_sig >= sigma_min && rv_sig <= sigma_max) {
                std::lock_guard<std::mutex> lk(hot.mu);
                hot.have_sigma = true;
                hot.sigma_bucket = rv_sig;
              }
            }
          }
        }

        const std::int64_t t_sigma_end =
            spot_latency_monitor ? ll::core::steady_ns() : std::int64_t{0};
        on_hot_update();
        const std::int64_t t_spot_end =
            spot_latency_monitor ? ll::core::steady_ns() : std::int64_t{0};
        if (spot_latency_monitor) {
          spot_lat_total_ns.push_back(t_spot_end - t_spot_begin);
          spot_lat_sigma_ns.push_back(t_sigma_end - t_sigma_begin);
          spot_lat_q_rem.push_back(static_cast<std::int64_t>(q_rem));
          constexpr std::size_t kSpotLatCap = 16384;
          if (spot_lat_total_ns.size() > kSpotLatCap) {
            const auto drop = static_cast<std::ptrdiff_t>(kSpotLatCap / 2);
            spot_lat_total_ns.erase(spot_lat_total_ns.begin(), spot_lat_total_ns.begin() + drop);
            spot_lat_sigma_ns.erase(spot_lat_sigma_ns.begin(), spot_lat_sigma_ns.begin() + drop);
            spot_lat_q_rem.erase(spot_lat_q_rem.begin(), spot_lat_q_rem.begin() + drop);
          }
        }
        continue;
      }

      if (ev.type == Event::Type::PolyQuote) {
        {
          std::lock_guard<std::mutex> lk(hot.mu);
          if (ev.quote.outcome == "Up") {
            hot.have_up = true;
            hot.up_bid = ev.quote.best_bid;
            hot.up_ask = ev.quote.best_ask;
          } else if (ev.quote.outcome == "Down") {
            hot.have_down = true;
            hot.down_bid = ev.quote.best_bid;
            hot.down_ask = ev.quote.best_ask;
          }
          if (ev.quote.market_bucket_epoch >= 0 && hot.active_epoch < 0) {
            hot.active_epoch = ev.quote.market_bucket_epoch;
            hot.slug = ev.quote.event_slug;
            hot.edge_up_ok_since_mono_ns = -1;
            hot.edge_dn_ok_since_mono_ns = -1;
          }
        }
        on_hot_update();
        continue;
      }
    }
  });

  if (spot_feed_chainlink) {
    ll::polymarket::RtdsChainlinkFeed::instance().set_on_tick(
        [&](std::int64_t ts_ms, double px) {
          Event e;
          e.type = Event::Type::ChainlinkSpot;
          e.chainlink_ts_ms = ts_ms;
          e.chainlink_px = px;
          push_event(std::move(e));
        });
    ll::polymarket::RtdsChainlinkFeed::instance().ensure_started();
  } else {
    // Binance callback: update mid + hist; snapshot sigma once per bucket after open (strike K from Gamma).
    bin_client->set_on_bookticker([&](const ll::core::BookTickerTick& b) {
      Event e;
      e.type = Event::Type::BinBook;
      e.book = b;
      push_event(std::move(e));
    });

    bin_client->set_on_trade([&](const ll::core::TradeTick& t) {
      Event e;
      e.type = Event::Type::BinTrade;
      e.trade = t;
      push_event(std::move(e));
    });
  }

  // Polymarket quote callbacks: update best bid/ask.
  feed_up.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
    Event e;
    e.type = Event::Type::PolyQuote;
    e.quote = q;
    push_event(std::move(e));
  });
  feed_down.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
    Event e;
    e.type = Event::Type::PolyQuote;
    e.quote = q;
    push_event(std::move(e));
  });

  // Consumer thread now owns hot updates; callbacks only enqueue. Disable direct hot updates.

  std::thread bin_thread;
  if (!spot_feed_chainlink) {
    bin_thread = std::thread([&] {
      if (!bin_client->start(bin_cfg)) {
        { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][binance] start failed\n"; }
        g_stop = true;
      }
      while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      bin_client->stop();
    });
  }

  // Polymarket discovery + rollover.
  std::int64_t active_epoch = -1;
  std::int64_t next_epoch = -1;
  std::string current_slug;
  std::string token_up;
  std::string token_dn;

  ll::polymarket::BtcFiveMinuteBucketDiscovery initial_disc;
  if (!poly_discover) {
    std::string derr;
    if (!ll::polymarket::discover_btc_updown_5m_for_exact_slug(poly_event_slug, initial_disc, &derr)) {
      { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] gamma slug fetch failed: " << derr << "\n"; }
      g_stop = true;
    } else if (initial_disc.up_token_id.empty() || initial_disc.down_token_id.empty()) {
      { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] discovery missing up/down token id\n"; }
      g_stop = true;
    } else if (poly_manual_token != initial_disc.up_token_id) {
      { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] --poly-token must match Gamma Up token id for slug\n"; }
      g_stop = true;
    } else {
      token_up = initial_disc.up_token_id;
      token_dn = initial_disc.down_token_id;
      current_slug = initial_disc.confirmed_slug;
      active_epoch = initial_disc.bucket_epoch_seconds;
      if (active_epoch < 0 && !parse_epoch_from_confirmed_slug(current_slug, &active_epoch)) {
        { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] bucket epoch unavailable from slug\n"; }
        g_stop = true;
      } else {
        maybe_fill_strike_web_then_rtds(initial_disc, poly_rollover_web_ptb, "[paper_trader]");
        feed_up.set_market_context(current_slug, active_epoch, "Up");
        feed_down.set_market_context(current_slug, active_epoch, "Down");
        std::string e1, e2;
        if (!feed_up.start(token_up, &e1) || !feed_down.start(token_dn, &e2)) {
          { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] ws start failed: " << e1 << " " << e2 << "\n"; }
          g_stop = true;
        } else {
          ll::polymarket::RtdsChainlinkFeed::instance().ensure_started();
          {
            std::lock_guard<std::mutex> hk(hot.mu);
            hot.slug = current_slug;
            hot.active_epoch = active_epoch;
            if (bucket_disc_has_strike(initial_disc)) {
              hot.have_K = true;
              hot.K = initial_disc.price_to_beat;
            } else {
              hot.have_K = false;
            }
          }
          trading_enabled = true;
        }
      }
    }
  } else {
    std::string derr;
    std::string err_exact;
    const std::int64_t wall_now_s = ll::core::system_ms() / 1000;
    const std::int64_t utc_floor = (wall_now_s / 300) * 300;
    const std::string slug_exact = "btc-updown-5m-" + std::to_string(utc_floor);
    bool discovered = false;
    // Same order as live_pipeline rollover: exact slug for the expected epoch, then ± bucket scan.
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
        { ll::io::SyncCerrLock _;
          std::cerr << "[paper_trader][poly] bucket discovery failed: exact_slug_err=" << err_exact
                    << " scan_err=" << derr << "\n"; }
        g_stop = true;
      }
    }
    if (!g_stop.load(std::memory_order_relaxed) &&
        (initial_disc.up_token_id.empty() || initial_disc.down_token_id.empty())) {
      { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] discovery missing up/down token id\n"; }
      g_stop = true;
    }
    if (!g_stop.load(std::memory_order_relaxed)) {
      token_up = initial_disc.up_token_id;
      token_dn = initial_disc.down_token_id;
      current_slug = initial_disc.confirmed_slug;
      active_epoch = initial_disc.bucket_epoch_seconds;
      if (active_epoch < 0 && !parse_epoch_from_confirmed_slug(current_slug, &active_epoch)) {
        { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] bucket epoch unavailable from slug\n"; }
        g_stop = true;
      } else {
        next_epoch = active_epoch + 300;
        maybe_fill_strike_web_then_rtds(initial_disc, poly_rollover_web_ptb, "[paper_trader]");
        feed_up.set_market_context(current_slug, active_epoch, "Up");
        feed_down.set_market_context(current_slug, active_epoch, "Down");
        std::string e1, e2;
        if (!feed_up.start(token_up, &e1) || !feed_down.start(token_dn, &e2)) {
          { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] ws start failed: " << e1 << " " << e2 << "\n"; }
          g_stop = true;
        } else {
          ll::polymarket::RtdsChainlinkFeed::instance().ensure_started();
          {
            std::lock_guard<std::mutex> hk(hot.mu);
            hot.slug = current_slug;
            hot.active_epoch = active_epoch;
            if (bucket_disc_has_strike(initial_disc)) {
              hot.have_K = true;
              hot.K = initial_disc.price_to_beat;
            } else {
              hot.have_K = false;
            }
          }
          // hot state will be reset/updated by the single consumer thread via events.
          {
            ll::io::SyncCerrLock _;
            std::cerr << '[' << src_tag << "] warmup: first slug will NOT trade; will enable on rollover slug="
                      << current_slug << " active_epoch=" << active_epoch << " price_to_beat=";
            if (initial_disc.gamma_has_price_to_beat && bucket_disc_has_strike(initial_disc)) {
              std::cerr << initial_disc.price_to_beat << " (gamma_metadata)";
            } else if (initial_disc.strike_from_polymarket_web_event_page) {
              std::cerr << initial_disc.price_to_beat << " (polymarket_web_event_page)";
            } else if (initial_disc.strike_from_polymarket_rtds_chainlink) {
              std::cerr << initial_disc.price_to_beat << " (polymarket_rtds_chainlink)";
            } else {
              std::cerr << "(strike_unavailable)";
            }
            std::cerr << '\n';
          }
        }
      }
    }
  }

  // Rollover monitor: when next bucket is ready, switch tokens; FORCE_SELL at rollover.
  while (!g_stop.load(std::memory_order_relaxed) && poly_discover) {
    const auto now_s = ll::core::system_ms() / 1000;
    if (active_epoch >= 0 && now_s >= next_epoch + 1) {
      ll::polymarket::BtcFiveMinuteBucketDiscovery d2;
      std::string err_exact;
      std::string err_scan;
      const std::string slug_next = "btc-updown-5m-" + std::to_string(next_epoch);
      bool got = false;
      if (ll::polymarket::discover_btc_updown_5m_for_exact_slug(slug_next, d2, &err_exact)) {
        std::int64_t ep = d2.bucket_epoch_seconds;
        if (ep < 0 && !parse_epoch_from_confirmed_slug(d2.confirmed_slug, &ep)) {
          ep = -1;
        }
        if (ep >= next_epoch) {
          got = true;
        }
      }
      if (!got && ll::polymarket::discover_active_btc_updown_5m_via_bucket(d2, &err_scan) &&
          d2.bucket_epoch_seconds >= next_epoch) {
        got = true;
      }
      if (got) {
        maybe_fill_strike_web_then_rtds(d2, poly_rollover_web_ptb, "[paper_trader]");
        // FORCE_SELL is handled by the single consumer thread (on rollover event).
        // Snapshot OLD-slug quotes BEFORE switch_token: after switch, hot may be empty/stale for the new tokens.
        Event rollover_ev;
        rollover_ev.type = Event::Type::Rollover;
        rollover_ev.rollover_epoch = d2.bucket_epoch_seconds;
        rollover_ev.rollover_slug = d2.confirmed_slug;
        rollover_ev.rollover_price_to_beat =
            bucket_disc_has_strike(d2) ? d2.price_to_beat : std::numeric_limits<double>::quiet_NaN();
        {
          std::lock_guard<std::mutex> hk(hot.mu);
          rollover_ev.rollover_up_bid = hot.up_bid;
          rollover_ev.rollover_up_ask = hot.up_ask;
          rollover_ev.rollover_down_bid = hot.down_bid;
          rollover_ev.rollover_down_ask = hot.down_ask;
        }

        std::string err_up, err_dn;
        if (feed_up.switch_token(d2.up_token_id, &err_up) &&
            feed_down.switch_token(d2.down_token_id, &err_dn)) {
          current_slug = d2.confirmed_slug;
          active_epoch = d2.bucket_epoch_seconds;
          if (active_epoch < 0 && !parse_epoch_from_confirmed_slug(current_slug, &active_epoch)) {
            { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][rollover] epoch missing from slug\n"; }
          }
          next_epoch = active_epoch + 300;
          feed_up.set_market_context(current_slug, active_epoch, "Up");
          feed_down.set_market_context(current_slug, active_epoch, "Down");
          push_event(std::move(rollover_ev));
          {
            ll::io::SyncCerrLock _;
            std::cerr << '[' << src_tag << "][rollover] active_epoch=" << active_epoch << " slug=" << current_slug
                      << " price_to_beat=";
            if (d2.gamma_has_price_to_beat && bucket_disc_has_strike(d2)) {
              std::cerr << d2.price_to_beat << " (gamma_metadata)";
            } else if (d2.strike_from_polymarket_web_event_page) {
              std::cerr << d2.price_to_beat << " (polymarket_web_event_page)";
            } else if (d2.strike_from_polymarket_rtds_chainlink) {
              std::cerr << d2.price_to_beat << " (polymarket_rtds_chainlink)";
            } else {
              std::cerr << "(strike_unavailable)";
            }
            std::cerr << '\n';
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  feed_up.stop();
  if (poly_discover) feed_down.stop();
  if (bin_thread.joinable()) bin_thread.join();
  // Stop consumer thread after feeds stop.
  {
    Event e;
    e.type = Event::Type::Stop;
    push_event(std::move(e));
  }
  if (consumer_thr.joinable()) consumer_thr.join();
  { ll::io::SyncCerrLock _; std::cerr << "telemetry: " << tel.summary() << "\n"; }
  return 0;
}

}  // namespace

namespace ll::btc_poly {

int run_strategy_main(int argc, char** argv, bool live_execution) {
#ifndef LL_ENABLE_LIVE_TRADER
  if (live_execution) {
    { ll::io::SyncCerrLock _; std::cerr << "[btc_poly] live execution requires CMake -DBUILD_LIVE_TRADER=ON\n"; }
    return 2;
  }
#endif
  return run_live_impl(live_execution, argc, argv);
}

}  // namespace ll::btc_poly
