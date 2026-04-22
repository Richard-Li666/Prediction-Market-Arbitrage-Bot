#pragma once

#include <cstdint>
#include <string>

namespace ll::polymarket {

/// Normalized top-of-book update from websocket (`WsFixedTokenQuoteFeed`).
struct PolymarketWsQuote {
  std::string token_id;
  /// Gamma-confirmed slug for the market (e.g. `btc-updown-5m-<epoch>`); may be empty if unknown.
  std::string event_slug;
  /// Parsed bucket start from slug when known; otherwise -1.
  std::int64_t market_bucket_epoch{-1};
  /// Outcome label for `token_id` (e.g. "Up" / "Down"); may be empty if unknown.
  std::string outcome;
  double best_bid{0};
  double best_ask{0};
  int64_t local_mono_ns{-1};
  int64_t local_wall_ms{-1};
};

struct PolymarketQuoteTick {
  std::string token_id;
  std::string outcome;
  std::string market_slug;
  std::string market_id;
  double best_bid{0};
  double best_ask{0};
  int64_t local_mono_ns{-1};
  int64_t local_wall_ms{-1};
};

struct PolymarketMarketRef {
  /// Gamma numeric market id (stringified), when known.
  std::string market_id;
  /// Condition id (`0x...`), matches websocket `market` field.
  std::string condition_id;
  std::string slug;
  std::string up_token_id;
  std::string down_token_id;
  std::string start_time;
  std::string end_time;
  bool active{false};
};

}  // namespace ll::polymarket
