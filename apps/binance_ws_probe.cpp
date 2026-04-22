#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "binance/stream_client.hpp"
#include "binance/stream_env.hpp"
#include "telemetry/pipeline.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_sig(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, on_sig);
  std::signal(SIGTERM, on_sig);

  ll::binance::StreamClientConfig cfg;
  ll::binance::apply_stream_env_overrides(cfg);
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--host" && i + 1 < argc) {
      cfg.ws_host = argv[++i];
    } else if (a == "--port" && i + 1 < argc) {
      cfg.ws_port = std::stoi(argv[++i]);
    } else if (a == "--stream" && i + 1 < argc) {
      const std::string m = argv[++i];
      if (m == "trade") {
        cfg.stream_path = "/ws/btcusdt@trade";
      } else if (m == "bookTicker") {
        cfg.stream_path = "/ws/btcusdt@bookTicker";
      } else {
        std::cerr << "--stream must be trade or bookTicker\n";
        return 2;
      }
    } else if (a == "--help" || a == "-h") {
      std::cerr << "usage: binance_ws_probe [--host HOST] [--port PORT] [--stream trade|bookTicker]\n"
                << "  default stream: btcusdt@bookTicker\n"
                << "  global host: LL_BINANCE_WS_HOST=stream.binance.com binance_ws_probe\n";
      return 0;
    } else {
      std::cerr << "unknown arg: " << a << " (try --help)\n";
      return 2;
    }
  }

  ll::telemetry::Pipeline tel;
  ll::binance::StreamClient client(&tel);
  client.set_on_bookticker([](const ll::core::BookTickerTick& b) {
    std::cout << "local_mono_ns=" << b.local_mono_ns << " bid=" << b.bid_price << " bid_qty=" << b.bid_qty
              << " ask=" << b.ask_price << " ask_qty=" << b.ask_qty << " update_id=" << b.update_id << "\n";
  });
  client.set_on_trade([](const ll::core::TradeTick& t) {
    std::cout << "local_mono_ns=" << t.local_mono_ns << " exchange_ms=" << t.exchange_ts_ms
              << " px=" << t.price << " qty=" << t.qty << " id=" << t.trade_id << "\n";
  });

  std::cerr << "[binance_ws] connecting to wss://" << cfg.ws_host << ':' << cfg.ws_port << cfg.stream_path
            << "\n";

  if (!client.start(cfg)) {
    std::cerr << "failed to start binance stream\n";
    return 1;
  }

  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  client.stop();
  std::cout << "telemetry: " << tel.summary() << "\n";
  return 0;
}
