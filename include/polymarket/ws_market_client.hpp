#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace ll::polymarket {

/// Thin CLOB market-channel WebSocket client (`wss://ws-subscriptions-clob.polymarket.com/ws/market`).
class MarketWsClient {
 public:
  MarketWsClient();
  ~MarketWsClient();

  void set_on_open(std::function<void()> cb);
  void set_on_message(std::function<void(const std::string&)> cb);
  void set_on_closed(std::function<void()> cb);

  /// Connect and start periodic `PING` texts (market channel heartbeat).
  bool start();
  void stop();

  void send_json(const nlohmann::json& msg);

  /// Replace the reconnect subscription list (not merged). On each successful TCP open, the client sends *only*
  /// these asset ids as the baseline `market` subscribe — stale ids must not remain here after rollover.
  void set_subscription_snapshot(std::vector<std::string> asset_ids);

  MarketWsClient(const MarketWsClient&) = delete;
  MarketWsClient& operator=(const MarketWsClient&) = delete;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ll::polymarket
