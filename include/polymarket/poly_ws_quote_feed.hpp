#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "polymarket/gamma_bootstrap.hpp"
#include "polymarket/polymarket_types.hpp"
#include "polymarket/ws_market_client.hpp"

namespace ll::telemetry {
class Pipeline;
}

namespace ll::polymarket {

struct PolyWsQuoteFeedConfig {
  GammaBootstrapParams gamma;
  /// If set, skip Gamma and subscribe to this Up token id (rollover still works when `new_market` is received).
  std::optional<std::string> initial_up_token_id;
  /// `new_market.slug` must start with this prefix (case-insensitive); default matches Gamma BTC 5m Up/Down family.
  std::string rollover_slug_prefix{"btc-updown-5m"};
};

class PolyWsQuoteFeed {
 public:
  explicit PolyWsQuoteFeed(telemetry::Pipeline* tel);
  ~PolyWsQuoteFeed();

  PolyWsQuoteFeed(const PolyWsQuoteFeed&) = delete;
  PolyWsQuoteFeed& operator=(const PolyWsQuoteFeed&) = delete;

  void set_config(PolyWsQuoteFeedConfig cfg);

  using QuoteFn = std::function<void(const PolymarketQuoteTick&)>;
  using RolloverFn = std::function<void(const PolymarketMarketRef& old_ref, const PolymarketMarketRef& new_ref)>;

  void set_on_quote(QuoteFn fn);
  void set_on_rollover(RolloverFn fn);

  /// Bootstrap (optional HTTP) + start the CLOB market websocket. Subscribes to the Up token.
  bool start(std::string* error_message);
  void stop();

  const PolymarketMarketRef& active_market() const { return active_; }
  const std::string& active_up_token() const { return active_up_token_; }
  std::uint64_t messages_received() const { return msg_count_; }

 private:
  /// Drop websocket quote rows whose `asset_id` is not exactly the current Up token (stale rows after rollover).
  bool guard_active_quote_asset(const std::string& asset_id) const;

  void on_ws_message(const std::string& s);
  void on_ws_open();
  void on_ws_closed();
  void try_hydrate_from_message(const nlohmann::json& j);
  bool parse_top_of_book_from_book(const nlohmann::json& j, double* bid, double* ask) const;
  bool maybe_handle_new_market(const nlohmann::json& j);
  bool new_market_matches_family(const nlohmann::json& j) const;
  PolymarketMarketRef market_ref_from_new_market(const nlohmann::json& j) const;
  void switch_to_market(const PolymarketMarketRef& next, std::uint64_t announce_ts_ms);
  void emit_quote(const std::string& quote_asset_id, double bid, double ask, const std::string& dedup_ts);

  telemetry::Pipeline* tel_;
  PolyWsQuoteFeedConfig cfg_;
  MarketWsClient ws_;

  QuoteFn on_quote_;
  RolloverFn on_rollover_;

  PolymarketMarketRef active_;
  std::string active_up_token_;
  std::uint64_t active_announced_ts_ms_{0};
  std::string last_quote_dedup_;
  std::uint64_t msg_count_{0};
  bool running_{false};
};

}  // namespace ll::polymarket
