#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

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

void append_quote_row(ll::logging::JsonlWriter& writer, std::atomic<std::uint64_t>& seq,
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

void append_market_rollover_row(ll::logging::JsonlWriter& writer, const std::string& old_slug,
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

  std::string token;
  std::string out_path;
  bool discover_mode = false;
  ll::polymarket::BtcFiveMinuteBucketDiscovery initial_disc;
  std::int64_t active_epoch = -1;
  std::int64_t next_epoch = -1;
  std::string next_up_token;
  std::string next_down_token;
  std::string next_confirmed_slug;
  bool next_ready = false;
  auto last_prefetch_mono = std::chrono::steady_clock::now() -
                            std::chrono::seconds(10);
  constexpr auto kPrefetchInterval = std::chrono::seconds(3);
  std::string current_slug;

  if (argc >= 2 && std::string(argv[1]) == "--discover") {
    if (argc < 3) {
      std::cerr << "usage: polymarket_logger --discover <out.jsonl>\n";
      return 2;
    }
    out_path = argv[2];
    std::string derr;
    if (!ll::polymarket::discover_active_btc_updown_5m_via_bucket(initial_disc, &derr)) {
      std::cerr << "bucket discovery failed: " << derr << "\n";
      return 1;
    }
    if (initial_disc.up_token_id.empty() || initial_disc.down_token_id.empty()) {
      std::cerr << "bucket discovery missing up or down token id\n";
      return 1;
    }
    discover_mode = true;
    token = initial_disc.up_token_id;
    active_epoch = initial_disc.bucket_epoch_seconds;
    if (active_epoch < 0 &&
        !parse_epoch_from_confirmed_slug(initial_disc.confirmed_slug, &active_epoch)) {
      std::cerr << "bucket discovery missing bucket_epoch_seconds and slug parse failed\n";
      return 1;
    }
    next_epoch = active_epoch + 300;
    current_slug = initial_disc.confirmed_slug;
    std::cerr << "[polymarket_logger] discovered slug=" << initial_disc.confirmed_slug
              << " up_token=" << initial_disc.up_token_id
              << " down_token=" << initial_disc.down_token_id << " condition=" << initial_disc.condition_id
              << "\n";
  } else {
    if (argc < 3) {
      std::cerr << "usage: polymarket_logger <token_id> <out.jsonl>\n"
                   "       polymarket_logger --discover <out.jsonl>\n";
      return 2;
    }
    token = argv[1];
    out_path = argv[2];
  }

  ll::telemetry::Pipeline tel;
  ll::logging::JsonlWriter writer(out_path);
  std::atomic<std::uint64_t> seq{0};

  ll::polymarket::WsFixedTokenQuoteFeed feed_up(&tel);
  ll::polymarket::WsFixedTokenQuoteFeed feed_down(&tel);

  if (discover_mode) {
    feed_up.set_market_context(current_slug, active_epoch, "Up");
    feed_down.set_market_context(current_slug, active_epoch, "Down");
    feed_up.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
      append_quote_row(writer, seq, q);
    });
    feed_down.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
      append_quote_row(writer, seq, q);
    });
    std::string err_up;
    std::string err_dn;
    if (!feed_up.start(initial_disc.up_token_id, &err_up)) {
      std::cerr << "start (up) failed: " << err_up << "\n";
      return 1;
    }
    if (!feed_down.start(initial_disc.down_token_id, &err_dn)) {
      std::cerr << "start (down) failed: " << err_dn << "\n";
      feed_up.stop();
      return 1;
    }
  } else {
    feed_up.set_market_context("", -1, "Up");
    feed_up.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
      append_quote_row(writer, seq, q);
    });
    std::string err;
    if (!feed_up.start(token, &err)) {
      std::cerr << "start failed: " << err << "\n";
      return 1;
    }
  }

  while (!g_stop.load()) {
    if (discover_mode) {
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
            std::cerr << "[prefetch] next slug=" << nd.confirmed_slug << " up_token=" << next_up_token
                      << " down_token=" << next_down_token << "\n";
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
              std::cerr << "[rollover] bucket fallback missing epoch: " << e2 << "\n";
            }
          } else {
            std::cerr << "[rollover] discovery failed (exact then bucket): " << e2 << "\n";
          }
        }

        if (!new_up.empty() && !new_down.empty() && new_active >= 0 && !new_slug.empty()) {
          const std::string old_slug = current_slug;
          const std::string old_up = feed_up.token_id();
          const std::string old_down = feed_down.token_id();

          std::string err_up;
          std::string err_dn;
          if (!feed_up.switch_token(new_up, &err_up)) {
            std::cerr << "[rollover] switch_token (up) failed: " << err_up << "\n";
          } else if (!feed_down.switch_token(new_down, &err_dn)) {
            std::cerr << "[rollover] switch_token (down) failed: " << err_dn << "\n";
          } else {
            feed_up.set_market_context(new_slug, new_active, "Up");
            feed_down.set_market_context(new_slug, new_active, "Down");
            append_market_rollover_row(writer, old_slug, new_slug, old_up, new_up, old_down, new_down);
            active_epoch = new_active;
            next_epoch = active_epoch + 300;
            current_slug = new_slug;
            next_ready = false;
            next_up_token.clear();
            next_down_token.clear();
            next_confirmed_slug.clear();
            std::cerr << "[rollover] active_epoch=" << active_epoch << " slug=" << new_slug
                      << " up_token=" << new_up << " down_token=" << new_down << "\n";
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  feed_up.stop();
  if (discover_mode) {
    feed_down.stop();
  }
  writer.flush();
  std::cerr << "telemetry: " << tel.summary() << "\n";
  return 0;
}
