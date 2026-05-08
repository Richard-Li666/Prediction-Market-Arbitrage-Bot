#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ll::polymarket {

/// Single long-lived RTDS `crypto_prices_chainlink` (`btc/usd`) connection + tick ring buffer.
/// Avoids reconnect-per-strike so the first tick at/after bucket boundary is less likely to be missed.
class RtdsChainlinkFeed {
 public:
  static RtdsChainlinkFeed& instance();

  void ensure_started();
  void stop();

  /// Optional: invoked on the websocket thread for each parsed `(payload_timestamp_ms, value)` tick.
  /// Use for live spot pricing; does not replace internal buffering / `wait_strike`.
  void set_on_tick(std::function<void(std::int64_t payload_ts_ms, double px)> cb);

  /// Blocks until a strike can be chosen from buffered + live ticks (same rules as RTDS strike fill).
  /// Only one wait runs at a time (serialized).
  bool wait_strike(std::int64_t boundary_ms, std::int64_t max_skew_ms, std::int64_t timeout_ms, double* out_px,
                   std::int64_t* out_payload_ts, const char** out_pick, bool* out_immediate,
                   std::string* error_message);

 private:
  RtdsChainlinkFeed() = default;
};

}  // namespace ll::polymarket
