#include "polymarket/ws_market_client.hpp"

#include <chrono>
#include <mutex>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

namespace ll::polymarket {

namespace {

std::once_flag net_once;

void ensure_net() {
  std::call_once(net_once, [] { ix::initNetSystem(); });
}

void ping_worker(std::atomic<bool>* stop_flag, ix::WebSocket* ws) {
  while (!stop_flag->load()) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    if (stop_flag->load()) {
      break;
    }
    ws->sendText(std::string("PING"));
  }
}

}  // namespace

struct MarketWsClient::Impl {
  ix::WebSocket ws;
  std::atomic<bool> running{false};
  std::atomic<bool> ping_stop{false};
  std::thread ping_thr;
  std::function<void()> user_on_open;
  std::function<void(const std::string&)> user_on_msg;
  std::function<void()> user_on_closed;
  std::mutex snapshot_mu;
  std::vector<std::string> snapshot_ids;

  /// Deterministic reconnect: one baseline subscribe built solely from `snapshot_ids` (current active token only).
  void send_snapshot_subscribe_if_any() {
    std::vector<std::string> ids;
    {
      std::lock_guard<std::mutex> lk(snapshot_mu);
      ids = snapshot_ids;
    }
    if (ids.empty()) {
      return;
    }
    nlohmann::json sub;
    sub["assets_ids"] = ids;
    sub["type"] = "market";
    sub["custom_feature_enabled"] = true;
    ws.sendText(sub.dump());
  }
};

MarketWsClient::MarketWsClient() : impl_(std::make_unique<Impl>()) {}

MarketWsClient::~MarketWsClient() { stop(); }

void MarketWsClient::set_on_open(std::function<void()> cb) { impl_->user_on_open = std::move(cb); }

void MarketWsClient::set_on_message(std::function<void(const std::string&)> cb) {
  impl_->user_on_msg = std::move(cb);
}

void MarketWsClient::set_on_closed(std::function<void()> cb) { impl_->user_on_closed = std::move(cb); }

void MarketWsClient::send_json(const nlohmann::json& msg) { impl_->ws.sendText(msg.dump()); }

void MarketWsClient::set_subscription_snapshot(std::vector<std::string> asset_ids) {
  std::lock_guard<std::mutex> lk(impl_->snapshot_mu);
  impl_->snapshot_ids = std::move(asset_ids);
}

bool MarketWsClient::start() {
  ensure_net();
  if (impl_->running.load()) {
    return true;
  }

  impl_->ws.setUrl("wss://ws-subscriptions-clob.polymarket.com/ws/market");
  impl_->ws.enableAutomaticReconnection();

  impl_->ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    if (!msg) {
      return;
    }
    if (msg->type == ix::WebSocketMessageType::Open) {
      impl_->send_snapshot_subscribe_if_any();
      if (impl_->user_on_open) {
        impl_->user_on_open();
      }
      return;
    }
    if (msg->type == ix::WebSocketMessageType::Close) {
      if (impl_->user_on_closed) {
        impl_->user_on_closed();
      }
      return;
    }
    if (msg->type == ix::WebSocketMessageType::Error) {
      return;
    }
    if (msg->type != ix::WebSocketMessageType::Message) {
      return;
    }
    const std::string& s = msg->str;
    if (s == "PONG") {
      return;
    }
    if (impl_->user_on_msg) {
      impl_->user_on_msg(s);
    }
  });

  impl_->ping_stop = false;
  impl_->ping_thr = std::thread(ping_worker, &impl_->ping_stop, &impl_->ws);

  impl_->ws.start();
  impl_->running = true;
  return true;
}

void MarketWsClient::stop() {
  if (!impl_->running.load()) {
    return;
  }
  impl_->ping_stop = true;
  if (impl_->ping_thr.joinable()) {
    impl_->ping_thr.join();
  }
  impl_->ws.stop();
  impl_->running = false;
}

}  // namespace ll::polymarket
