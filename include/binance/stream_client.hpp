#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "core/types.hpp"

namespace ll::core {
class ThreadPool;
}

namespace ll::telemetry {
class Pipeline;
}

namespace ll::binance {

struct StreamClientConfig {
  /// Spot streams host. Default is Binance.US (`stream.binance.us`) where `.com` may return 451.
  /// Override with `--host` or env `LL_BINANCE_WS_HOST`.
  std::string ws_host = "stream.binance.com";
  int ws_port = 9443;
  /// Raw stream path, e.g. `/ws/btcusdt@bookTicker` or `/ws/btcusdt@trade`.
  std::string stream_path = "/ws/btcusdt@bookTicker";
  std::size_t parse_workers = 0;
};

class StreamClient {
 public:
  using OnTrade = std::function<void(const core::TradeTick&)>;
  using OnBookTicker = std::function<void(const core::BookTickerTick&)>;

  explicit StreamClient(telemetry::Pipeline* telemetry = nullptr);
  ~StreamClient();

  void set_on_trade(OnTrade cb);
  void set_on_bookticker(OnBookTicker cb);

  bool start(const StreamClientConfig& cfg);
  void stop();

  StreamClient(const StreamClient&) = delete;
  StreamClient& operator=(const StreamClient&) = delete;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ll::binance
