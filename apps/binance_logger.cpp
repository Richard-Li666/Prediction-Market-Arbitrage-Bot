#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "binance/stream_client.hpp"
#include "binance/stream_env.hpp"
#include "logging/jsonl_writer.hpp"
#include "telemetry/pipeline.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_sig(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, on_sig);
  std::signal(SIGTERM, on_sig);

  if (argc < 2) {
    std::cerr << "usage: binance_logger <out.jsonl> [--host HOST] [--port P] [--stream trade|bookTicker] "
                 "[--parse-workers N]\n"
                 "  default: stream.binance.us + btcusdt@bookTicker\n"
                 "  bookTicker JSONL omits rows where best bid/ask prices are unchanged vs previous row.\n";
    return 2;
  }

  const std::string out_path = argv[1];
  std::size_t parse_workers = 0;
  ll::binance::StreamClientConfig cfg;
  ll::binance::apply_stream_env_overrides(cfg);
  for (int i = 2; i < argc; ++i) {
    if (std::string(argv[i]) == "--parse-workers" && i + 1 < argc) {
      parse_workers = static_cast<std::size_t>(std::stoul(argv[++i]));
    } else if (std::string(argv[i]) == "--host" && i + 1 < argc) {
      cfg.ws_host = argv[++i];
    } else if (std::string(argv[i]) == "--port" && i + 1 < argc) {
      cfg.ws_port = std::stoi(argv[++i]);
    } else if (std::string(argv[i]) == "--stream" && i + 1 < argc) {
      const std::string m = argv[++i];
      if (m == "trade") {
        cfg.stream_path = "/ws/btcusdt@trade";
      } else if (m == "bookTicker") {
        cfg.stream_path = "/ws/btcusdt@bookTicker";
      } else {
        std::cerr << "--stream must be trade or bookTicker\n";
        return 2;
      }
    }
  }

  ll::telemetry::Pipeline tel;
  ll::logging::JsonlWriter writer(out_path);

  std::atomic<std::uint64_t> seq{0};

  std::mutex book_dedup_mu;
  bool have_last_book{false};
  double last_bid_price{0};
  double last_ask_price{0};

  ll::binance::StreamClient client(&tel);
  cfg.parse_workers = parse_workers;

  std::cerr << "[binance_ws] connecting to wss://" << cfg.ws_host << ':' << cfg.ws_port << cfg.stream_path
            << "\n";

  client.set_on_bookticker([&](const ll::core::BookTickerTick& b) {
    std::lock_guard<std::mutex> lk(book_dedup_mu);
    if (have_last_book && b.bid_price == last_bid_price && b.ask_price == last_ask_price) {
      return;
    }
    have_last_book = true;
    last_bid_price = b.bid_price;
    last_ask_price = b.ask_price;
    const auto n = seq.fetch_add(1, std::memory_order_relaxed) + 1;
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
    writer.append(row);
  });
  client.set_on_trade([&](const ll::core::TradeTick& t) {
    const auto n = seq.fetch_add(1, std::memory_order_relaxed) + 1;
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
    writer.append(row);
  });

  if (!client.start(cfg)) {
    std::cerr << "failed to start binance stream\n";
    return 1;
  }

  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  client.stop();
  writer.flush();
  std::cerr << "telemetry: " << tel.summary() << "\n";
  return 0;
}
