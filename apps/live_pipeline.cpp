#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "binance/stream_client.hpp"
#include "binance/stream_env.hpp"
#include "core/clock.hpp"
#include "logging/jsonl_writer.hpp"
#include "polymarket/bucket_market_discovery.hpp"
#include "polymarket/ws_fixed_token_feed.hpp"
#include "telemetry/pipeline.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_sig(int) { g_stop = true; }

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

void append_poly_quote_row(ll::logging::JsonlWriter& writer, std::atomic<std::uint64_t>& seq,
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
  writer.append(row);
}

void append_poly_market_rollover_row(ll::logging::JsonlWriter& writer, const std::string& old_slug,
                                     const std::string& new_slug, const std::string& old_up,
                                     const std::string& new_up, const std::string& old_down,
                                     const std::string& new_down) {
  nlohmann::json row;
  row["schema_version"] = 1;
  row["source"] = "polymarket";
  row["event_type"] = "market_rollover";
  row["old_slug"] = old_slug;
  row["new_slug"] = new_slug;
  row["old_up_token_id"] = old_up;
  row["new_up_token_id"] = new_up;
  row["old_down_token_id"] = old_down;
  row["new_down_token_id"] = new_down;
  row["local_ts_mono_ns"] = ll::core::steady_ns();
  row["local_ts_wall_ms"] = ll::core::system_ms();
  writer.append(row);
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, on_sig);
  std::signal(SIGTERM, on_sig);
  int exit_code = 0;

  if (argc < 3) {
    std::cerr << "usage: live_pipeline <binance_out.jsonl> <polymarket_out.jsonl> [options]\n"
                 "  Binance bookTicker: JSONL only when best bid or ask price changes vs previous row.\n"
                 "  options:\n"
                 "    --stream trade|bookTicker   (binance; default: bookTicker via env)\n"
                 "    --binance-stream trade|bookTicker   (alias)\n"
                 "    --host HOST --port P        (binance websocket)\n"
                 "    --parse-workers N           (binance; 0 = default)\n"
                 "    --poly-discover             (default: on; Gamma bucket + dual WS + rollover)\n"
                 "    --poly-token TOKEN          (manual polymarket: single Up feed, no rollover)\n"
                 "    --poly-slug-prefix etc.     (accepted for compatibility; discovery unchanged)\n";
    return 2;
  }

  const std::string bin_out = argv[1];
  const std::string poly_out = argv[2];

  ll::binance::StreamClientConfig bin_cfg;
  bin_cfg.parse_workers = 2;
  ll::binance::apply_stream_env_overrides(bin_cfg);

  bool poly_discover = true;
  std::string poly_manual_token;

  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--poly-token" && i + 1 < argc) {
      poly_manual_token = argv[++i];
      poly_discover = false;
    } else if (a == "--poly-discover") {
      poly_discover = true;
      poly_manual_token.clear();
    } else if ((a == "--poly-slug-prefix" || a == "--poly-slug-filter" || a == "--poly-rollover-prefix" ||
                a == "--poly-rollover-slug") &&
               i + 1 < argc) {
      ++i;
    } else if (a == "--host" && i + 1 < argc) {
      bin_cfg.ws_host = argv[++i];
    } else if (a == "--port" && i + 1 < argc) {
      bin_cfg.ws_port = std::stoi(argv[++i]);
    } else if ((a == "--stream" || a == "--binance-stream") && i + 1 < argc) {
      const std::string m = argv[++i];
      if (m == "trade") {
        bin_cfg.stream_path = "/ws/btcusdt@trade";
      } else if (m == "bookTicker") {
        bin_cfg.stream_path = "/ws/btcusdt@bookTicker";
      } else {
        std::cerr << "--stream / --binance-stream must be trade or bookTicker\n";
        return 2;
      }
    } else if (a == "--parse-workers" && i + 1 < argc) {
      const auto n = std::stoul(argv[++i]);
      bin_cfg.parse_workers = n == 0 ? 0 : n;
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      return 2;
    }
  }

  ll::polymarket::BtcFiveMinuteBucketDiscovery initial_disc;
  std::int64_t active_epoch = -1;
  std::int64_t next_epoch = -1;
  std::string next_up_token;
  std::string next_down_token;
  std::string next_confirmed_slug;
  bool next_ready = false;
  auto last_prefetch_mono = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  constexpr auto kPrefetchInterval = std::chrono::seconds(3);
  std::string current_slug;

  if (poly_discover) {
    std::string derr;
    if (!ll::polymarket::discover_active_btc_updown_5m_via_bucket(initial_disc, &derr)) {
      std::cerr << "[live_pipeline] polymarket bucket discovery failed: " << derr << "\n";
      return 1;
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
    std::cerr << "[live_pipeline] polymarket discovered slug=" << initial_disc.confirmed_slug
              << " up_token=" << initial_disc.up_token_id << " down_token=" << initial_disc.down_token_id
              << "\n";
  }

  ll::telemetry::Pipeline tel;
  ll::logging::JsonlWriter bin_writer(bin_out);
  ll::logging::JsonlWriter poly_writer(poly_out);

  std::atomic<std::uint64_t> bin_seq{0};
  std::atomic<std::uint64_t> poly_seq{0};

  std::mutex bin_book_dedup_mu;
  bool bin_have_last_book{false};
  double bin_last_bid_price{0};
  double bin_last_ask_price{0};

  ll::binance::StreamClient bin_client(&tel);
  ll::polymarket::WsFixedTokenQuoteFeed feed_up(&tel);
  ll::polymarket::WsFixedTokenQuoteFeed feed_down(&tel);

  std::cerr << "[live_pipeline][binance_ws] connecting to wss://" << bin_cfg.ws_host << ':'
            << bin_cfg.ws_port << bin_cfg.stream_path << "\n";

  bin_client.set_on_bookticker([&](const ll::core::BookTickerTick& b) {
    std::lock_guard<std::mutex> lk(bin_book_dedup_mu);
    if (bin_have_last_book && b.bid_price == bin_last_bid_price && b.ask_price == bin_last_ask_price) {
      return;
    }
    bin_have_last_book = true;
    bin_last_bid_price = b.bid_price;
    bin_last_ask_price = b.ask_price;
    const auto n = bin_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    nlohmann::json row;
    row["schema_version"] = 1;
    row["source"] = "binance";
    row["symbol"] = b.symbol;
    row["exchange_ts_ms"] = nullptr;
    row["local_ts_mono_ns"] = b.local_mono_ns;
    row["local_ts_wall_ms"] = b.local_wall_ms;
    row["seq"] = n;
    row["event_type"] = "bookTicker";
    row["payload"] = {{"bid_price", b.bid_price},
                      {"bid_qty", b.bid_qty},
                      {"ask_price", b.ask_price},
                      {"ask_qty", b.ask_qty},
                      {"update_id", b.update_id}};
    bin_writer.append(row);
  });
  bin_client.set_on_trade([&](const ll::core::TradeTick& t) {
    const auto n = bin_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    nlohmann::json row;
    row["schema_version"] = 1;
    row["source"] = "binance";
    row["symbol"] = t.symbol;
    row["exchange_ts_ms"] = t.exchange_ts_ms;
    row["local_ts_mono_ns"] = t.local_mono_ns;
    row["local_ts_wall_ms"] = t.local_wall_ms;
    row["seq"] = n;
    row["event_type"] = "trade";
    row["payload"] = {{"price", t.price}, {"qty", t.qty}, {"trade_id", t.trade_id}};
    bin_writer.append(row);
  });

  std::atomic<int> bin_start_state{0};

  std::thread bin_thread([&] {
    if (!bin_client.start(bin_cfg)) {
      std::cerr << "[live_pipeline] binance start failed\n";
      bin_start_state.store(-1, std::memory_order_release);
      g_stop.store(true, std::memory_order_release);
      return;
    }
    bin_start_state.store(1, std::memory_order_release);
    while (!g_stop.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    bin_client.stop();
  });

  while (bin_start_state.load(std::memory_order_acquire) == 0 && !g_stop.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (bin_start_state.load(std::memory_order_acquire) < 0) {
    if (bin_thread.joinable()) {
      bin_thread.join();
    }
    bin_writer.flush();
    poly_writer.flush();
    std::cerr << "telemetry: " << tel.summary() << "\n";
    return 1;
  }

  if (poly_discover) {
    feed_up.set_market_context(current_slug, active_epoch, "Up");
    feed_down.set_market_context(current_slug, active_epoch, "Down");
    feed_up.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
      append_poly_quote_row(poly_writer, poly_seq, q);
    });
    feed_down.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
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
    }
  } else {
    if (poly_manual_token.empty()) {
      std::cerr << "[live_pipeline] --poly-token required when not using default discovery\n";
      exit_code = 1;
      g_stop.store(true, std::memory_order_release);
    } else {
      feed_up.set_market_context("", -1, "Up");
      feed_up.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
        append_poly_quote_row(poly_writer, poly_seq, q);
      });
      std::string err;
      if (!feed_up.start(poly_manual_token, &err)) {
        std::cerr << "[live_pipeline] polymarket start failed: " << err << "\n";
        exit_code = 1;
        g_stop.store(true, std::memory_order_release);
      }
    }
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
            next_ready = true;
            std::cerr << "[live_pipeline][prefetch] next slug=" << nd.confirmed_slug
                      << " up_token=" << next_up_token << " down_token=" << next_down_token << "\n";
          }
        }
      }

      if (wall_now >= active_epoch + 300) {
        if (next_ready && wall_now >= next_epoch + 300) {
          next_ready = false;
          next_up_token.clear();
          next_down_token.clear();
          next_confirmed_slug.clear();
        }

        std::string new_up;
        std::string new_down;
        std::string new_slug;
        std::int64_t new_active = -1;

        if (next_ready) {
          new_up = next_up_token;
          new_down = next_down_token;
          new_slug = next_confirmed_slug.empty() ? ("btc-updown-5m-" + std::to_string(next_epoch))
                                                  : next_confirmed_slug;
          new_active = next_epoch;
        } else {
          ll::polymarket::BtcFiveMinuteBucketDiscovery d2;
          std::string e2;
          const std::string slug_next = "btc-updown-5m-" + std::to_string(next_epoch);
          if (ll::polymarket::discover_btc_updown_5m_for_exact_slug(slug_next, d2, &e2)) {
            new_up = d2.up_token_id;
            new_down = d2.down_token_id;
            new_slug = d2.confirmed_slug;
            new_active = d2.bucket_epoch_seconds >= 0 ? d2.bucket_epoch_seconds : next_epoch;
          } else if (ll::polymarket::discover_active_btc_updown_5m_via_bucket(d2, &e2)) {
            new_up = d2.up_token_id;
            new_down = d2.down_token_id;
            new_slug = d2.confirmed_slug;
            if (d2.bucket_epoch_seconds >= 0) {
              new_active = d2.bucket_epoch_seconds;
            } else if (!parse_epoch_from_confirmed_slug(d2.confirmed_slug, &new_active)) {
              std::cerr << "[live_pipeline][rollover] bucket fallback missing epoch: " << e2 << "\n";
            }
          } else {
            std::cerr << "[live_pipeline][rollover] discovery failed (exact then bucket): " << e2 << "\n";
          }
        }

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
            append_poly_market_rollover_row(poly_writer, old_slug, new_slug, old_up, new_up, old_down,
                                             new_down);
            active_epoch = new_active;
            next_epoch = active_epoch + 300;
            current_slug = new_slug;
            next_ready = false;
            next_up_token.clear();
            next_down_token.clear();
            next_confirmed_slug.clear();
            std::cerr << "[live_pipeline][rollover] active_epoch=" << active_epoch << " slug=" << new_slug
                      << "\n";
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  feed_up.stop();
  if (poly_discover) {
    feed_down.stop();
  }

  if (bin_thread.joinable()) {
    bin_thread.join();
  }

  bin_writer.flush();
  poly_writer.flush();
  std::cerr << "telemetry: " << tel.summary() << "\n";
  return exit_code;
}
