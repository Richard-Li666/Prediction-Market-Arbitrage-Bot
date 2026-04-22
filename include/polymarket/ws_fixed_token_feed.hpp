#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "polymarket/polymarket_types.hpp"
#include "polymarket/ws_market_client.hpp"

namespace ll::telemetry {
class Pipeline;
}

namespace ll::polymarket {

/// WebSocket-only quote stream for a single CLOB asset id (no discovery, no rollover).
class WsFixedTokenQuoteFeed {
 public:
  explicit WsFixedTokenQuoteFeed(telemetry::Pipeline* tel);
  ~WsFixedTokenQuoteFeed();

  WsFixedTokenQuoteFeed(const WsFixedTokenQuoteFeed&) = delete;
  WsFixedTokenQuoteFeed& operator=(const WsFixedTokenQuoteFeed&) = delete;

  using QuoteFn = std::function<void(const PolymarketWsQuote&)>;

  void set_on_quote(QuoteFn fn);

  /// Optional labels copied into each `PolymarketWsQuote` (safe to call before `start` and while running).
  void set_market_context(const std::string& event_slug, std::int64_t market_bucket_epoch,
                          const std::string& outcome);

  /// Connect and subscribe to `token_id` only. Reconnect resubscribes the same id via `MarketWsClient` snapshot.
  bool start(const std::string& token_id, std::string* error_message);
  void stop();

  /// Unsubscribe previous asset and subscribe to `new_token_id` (same WS session). Clears quote dedup state.
  bool switch_token(const std::string& new_token_id, std::string* error_message);

  std::string token_id() const {
    std::lock_guard<std::mutex> lk(sub_mu_);
    return token_id_;
  }
  std::uint64_t messages_received() const { return msg_count_; }

 private:
  void on_ws_open();
  void on_ws_closed();
  void on_ws_message(const std::string& s);
  void emit_quote(const std::string& asset_id, double bid, double ask, const std::string& dedup_ts);

  telemetry::Pipeline* tel_;
  MarketWsClient ws_;

  QuoteFn on_quote_;
  mutable std::mutex sub_mu_;
  std::string token_id_;
  std::string quote_event_slug_;
  std::string quote_outcome_;
  std::int64_t quote_market_bucket_epoch_{-1};
  /// Suppress logger/probe callbacks when best bid/ask unchanged (exact double equality).
  bool have_last_emitted_quote_{false};
  double last_emitted_bid_{0};
  double last_emitted_ask_{0};
  std::uint64_t msg_count_{0};
  bool running_{false};
};

}  // namespace ll::polymarket
