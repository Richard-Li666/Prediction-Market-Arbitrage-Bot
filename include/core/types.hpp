#pragma once

#include <cstdint>
#include <string>

namespace ll::core {

struct TradeTick {
  int64_t exchange_ts_ms{-1};
  int64_t local_mono_ns{-1};
  int64_t local_wall_ms{-1};
  std::string symbol;
  double price{0};
  double qty{0};
  int64_t trade_id{-1};
};

/// Best bid/ask from Binance `bookTicker` stream (no exchange event time in struct).
struct BookTickerTick {
  std::string symbol;
  double bid_price{0};
  double bid_qty{0};
  double ask_price{0};
  double ask_qty{0};
  std::uint64_t update_id{0};
  int64_t local_mono_ns{-1};
  int64_t local_wall_ms{-1};
};

struct BookQuote {
  int64_t local_mono_ns{-1};
  int64_t local_wall_ms{-1};
  std::string token_id;
  double best_bid{0};
  double best_ask{0};
};

}  // namespace ll::core
