#include "binance/stream_client.hpp"

#include <iostream>
#include <mutex>
#include <sstream>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "core/clock.hpp"
#include "core/thread_pool.hpp"
#include "telemetry/pipeline.hpp"

namespace ll::binance {

namespace {

std::once_flag net_init_once;

void ensure_net() {
  std::call_once(net_init_once, [] { ix::initNetSystem(); });
}

std::uint64_t parse_bookticker_update_id(const nlohmann::json& j) {
  const auto it = j.find("u");
  if (it == j.end() || it->is_null()) {
    return 0;
  }
  if (it->is_number_unsigned()) {
    return it->get<std::uint64_t>();
  }
  if (it->is_number_integer()) {
    return static_cast<std::uint64_t>(it->get<std::int64_t>());
  }
  if (it->is_string()) {
    return std::stoull(it->get_ref<const std::string&>());
  }
  return 0;
}

/// Binance.US `bookTicker` may omit `e`; recognize by payload shape (same fields as global bookTicker).
bool looks_like_bookticker_payload(const nlohmann::json& j) {
  return j.contains("s") && j.contains("b") && j.contains("B") && j.contains("a") && j.contains("A") &&
         j.contains("u");
}

}  // namespace

struct StreamClient::Impl {
  ix::WebSocket ws;
  OnTrade on_trade;
  OnBookTicker on_bookticker;
  telemetry::Pipeline* tel = nullptr;
  std::atomic<bool> started{false};
  std::unique_ptr<core::ThreadPool> pool;
  StreamClientConfig cfg{};
};

StreamClient::StreamClient(telemetry::Pipeline* telemetry)
    : impl_(std::make_unique<Impl>()) {
  impl_->tel = telemetry;
}

StreamClient::~StreamClient() { stop(); }

void StreamClient::set_on_trade(OnTrade cb) { impl_->on_trade = std::move(cb); }

void StreamClient::set_on_bookticker(OnBookTicker cb) { impl_->on_bookticker = std::move(cb); }

bool StreamClient::start(const StreamClientConfig& cfg) {
  ensure_net();
  impl_->cfg = cfg;
  if (cfg.parse_workers > 0) {
    impl_->pool = std::make_unique<core::ThreadPool>(cfg.parse_workers);
  }

  std::ostringstream url;
  url << "wss://" << cfg.ws_host << ':' << cfg.ws_port << cfg.stream_path;
  const std::string url_str = url.str();
  impl_->ws.setUrl(url_str);

  impl_->ws.setOnMessageCallback([this, url_str](const ix::WebSocketMessagePtr& msg) {
    if (!msg) {
      return;
    }
    if (msg->type == ix::WebSocketMessageType::Open) {
      std::cerr << "[binance_ws] connected " << url_str << "\n";
      return;
    }
    if (msg->type == ix::WebSocketMessageType::Close) {
      std::cerr << "[binance_ws] closed code=" << msg->closeInfo.code << " reason=" << msg->closeInfo.reason
                << "\n";
      return;
    }
    if (msg->type == ix::WebSocketMessageType::Error) {
      std::cerr << "[binance_ws] error reason=" << msg->errorInfo.reason << " message=" << msg->str << "\n";
      return;
    }
    if (msg->type != ix::WebSocketMessageType::Message) {
      return;
    }

    const std::string& raw = msg->str;
    const int64_t recv_ns = core::steady_ns();
    if (impl_->tel) {
      impl_->tel->mark("binance_ws_recv", recv_ns);
    }

    auto handle = [this, raw, recv_ns]() {
      try {
        const auto j = nlohmann::json::parse(raw);

        if (looks_like_bookticker_payload(j)) {
          core::BookTickerTick bt;
          bt.symbol = j.value("s", std::string{});
          bt.bid_price = std::stod(j.at("b").get_ref<const std::string&>());
          bt.bid_qty = std::stod(j.at("B").get_ref<const std::string&>());
          bt.ask_price = std::stod(j.at("a").get_ref<const std::string&>());
          bt.ask_qty = std::stod(j.at("A").get_ref<const std::string&>());
          bt.update_id = parse_bookticker_update_id(j);
          bt.local_mono_ns = recv_ns;
          bt.local_wall_ms = core::system_ms();
          if (impl_->tel) {
            impl_->tel->mark("binance_parsed", core::steady_ns());
          }
          if (impl_->on_bookticker) {
            impl_->on_bookticker(bt);
          }
          return;
        }

        if (j.contains("e") && j["e"].is_string() && j["e"].get<std::string>() == "trade") {
          core::TradeTick tick;
          tick.exchange_ts_ms = j.value("E", int64_t{-1});
          tick.local_mono_ns = recv_ns;
          tick.local_wall_ms = core::system_ms();
          tick.symbol = j.value("s", std::string{});
          tick.price = std::stod(j.at("p").get_ref<const std::string&>());
          tick.qty = std::stod(j.at("q").get_ref<const std::string&>());
          tick.trade_id = j.value("t", int64_t{-1});
          if (impl_->tel) {
            impl_->tel->mark("binance_parsed", core::steady_ns());
          }
          if (impl_->on_trade) {
            impl_->on_trade(tick);
          }
        }
      } catch (...) {
      }
    };

    if (impl_->pool) {
      impl_->pool->enqueue(std::move(handle));
    } else {
      handle();
    }
  });

  impl_->ws.start();
  impl_->started = true;
  return true;
}

void StreamClient::stop() {
  if (!impl_->started.load()) {
    return;
  }
  impl_->ws.stop();
  if (impl_->pool) {
    impl_->pool->wait_idle();
    impl_->pool.reset();
  }
  impl_->started = false;
}

}  // namespace ll::binance
