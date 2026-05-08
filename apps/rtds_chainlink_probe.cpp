#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

namespace {

std::atomic<bool> g_stop{false};

void on_sig(int) { g_stop = true; }

std::int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void print_updates(const nlohmann::json& j, bool raw, std::int64_t boundary_ms) {
  auto one = [&](const nlohmann::json& o) {
    if (!o.is_object() || !o.contains("topic")) {
      return;
    }
    const std::string topic = o.value("topic", "");
    if (topic != "crypto_prices_chainlink") {
      return;
    }
    if (!o.contains("payload")) {
      return;
    }
    const auto& p = o["payload"];
    if (raw) {
      std::cout << "local_wall_ms=" << wall_ms() << ' ' << o.dump() << '\n';
      return;
    }
    if (p.contains("data") && p["data"].is_array()) {
      for (const auto& row : p["data"]) {
        if (!row.is_object()) {
          continue;
        }
        const std::int64_t pts = row.value("timestamp", std::int64_t{0});
        std::cout << "local_wall_ms=" << wall_ms() << " topic=" << topic << " symbol=" << row.value("symbol", "")
                  << " payload_ts_ms=" << pts << " value=" << row.value("value", 0.0);
        if (boundary_ms >= 0) {
          std::cout << " dt_from_boundary_ms=" << (pts - boundary_ms);
        }
        std::cout << '\n';
      }
      return;
    }
    if (p.contains("symbol")) {
      const std::int64_t pts = p.value("timestamp", std::int64_t{0});
      std::cout << "local_wall_ms=" << wall_ms() << " topic=" << topic << " symbol=" << p.value("symbol", "")
                << " payload_ts_ms=" << pts << " value=" << p.value("value", 0.0);
      if (boundary_ms >= 0) {
        std::cout << " dt_from_boundary_ms=" << (pts - boundary_ms);
      }
      std::cout << '\n';
    }
  };

  if (j.is_array()) {
    for (const auto& x : j) {
      one(x);
    }
    return;
  }
  one(j);
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, on_sig);
  std::signal(SIGTERM, on_sig);

  std::string symbol = "btc/usd";
  bool raw = false;
  std::int64_t boundary_ms = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--raw" || a == "-r") {
      raw = true;
    } else if (a == "--boundary-ms" && i + 1 < argc) {
      boundary_ms = std::stoll(argv[++i]);
    } else if (a == "--bucket-epoch-s" && i + 1 < argc) {
      boundary_ms = std::stoll(argv[++i]) * 1000;
    } else if (a == "--help" || a == "-h") {
      std::cerr << "usage: rtds_chainlink_probe [SYMBOL] [--raw] [--bucket-epoch-s EPOCH] [--boundary-ms MS]\n"
                << "  Polymarket RTDS Chainlink stream (same feed as strike): wss://ws-live-data.polymarket.com\n"
                << "  SYMBOL: btc/usd (default), eth/usd, sol/usd, ...\n"
                << "  --bucket-epoch-s: 5m slug suffix in seconds (boundary_ms = epoch * 1000); prints dt_from_boundary_ms\n"
                << "  --boundary-ms: explicit window start in unix ms (same as slug_epoch * 1000)\n"
                << "  --raw: print full JSON per message\n"
                << "  Ctrl+C to stop.\n";
      return 0;
    } else if (!a.empty() && a[0] != '-') {
      symbol = a;
    } else {
      std::cerr << "unknown arg: " << a << " (try --help)\n";
      return 2;
    }
  }

  ix::initNetSystem();
  ix::WebSocket ws;
  ws.setUrl("wss://ws-live-data.polymarket.com");

  ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    if (!msg || msg->type != ix::WebSocketMessageType::Message) {
      return;
    }
    try {
      const auto j = nlohmann::json::parse(msg->str);
      print_updates(j, raw, boundary_ms);
    } catch (...) {
    }
  });

  ws.start();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && ws.getReadyState() != ix::ReadyState::Open) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (ws.getReadyState() != ix::ReadyState::Open) {
    std::cerr << "rtds_chainlink_probe: websocket did not open in time\n";
    return 1;
  }

  nlohmann::json inner;
  inner["symbol"] = symbol;
  const std::string filter_str = inner.dump();
  nlohmann::json sub;
  sub["action"] = "subscribe";
  sub["subscriptions"] =
      nlohmann::json::array({{{"topic", "crypto_prices_chainlink"}, {"type", "*"}, {"filters", filter_str}}});
  ws.sendText(sub.dump());

  std::cerr << "[rtds_chainlink_probe] connected; filter symbol=" << symbol
            << (raw ? " (raw JSON)\n" : " (fields per line)\n");
  if (!raw && boundary_ms >= 0) {
    std::cerr << "[rtds_chainlink_probe] boundary_ms=" << boundary_ms << " (dt_from_boundary_ms = payload_ts_ms - boundary)\n";
  }

  std::thread ping_thr([&]() {
    while (!g_stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < 50; ++i) {
        if (g_stop.load(std::memory_order_relaxed)) {
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (ws.getReadyState() == ix::ReadyState::Open) {
        ws.sendText("PING");
      }
    }
  });

  while (!g_stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  ws.stop();
  if (ping_thr.joinable()) {
    ping_thr.join();
  }
  return 0;
}
