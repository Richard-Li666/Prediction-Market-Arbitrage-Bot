#pragma once

#include <cstdint>
#include <string>

namespace ll::polymarket {

struct BtcFiveMinuteBucketDiscovery;

/// When Gamma omits `eventMetadata.priceToBeat`, subscribe to Polymarket RTDS
/// `crypto_prices_chainlink` (`btc/usd`). Slug `btc-updown-5m-<epoch>` uses `epoch` seconds;
/// window start in ms is `epoch * 1000`, aligned with Chainlink `payload.timestamp` (ms).
/// Settlement-aligned strike (within `max_timestamp_skew_ms`):
/// 1) **Earliest** tick with `tick_ts >= event_start_wall_ms` — first oracle print at/after the window
///    boundary (matches Polymarket “Price to Beat” / Chainlink stream convention).
/// 2) If none arrives before `timeout_ms`, fall back to latest tick with `tick_ts <= target`, then
///    smallest `|tick_ts - target|` within skew.
///
/// Docs: https://docs.polymarket.com/developers/RTDS/RTDS-crypto-prices (no auth).
///
/// Note: RTDS is live streaming; if the process starts long after bucket open, buffered ticks may not
/// reach back to the window start — widen skew only if you accept reduced accuracy.
bool fill_strike_from_polymarket_rtds_chainlink(BtcFiveMinuteBucketDiscovery& disc,
                                                std::int64_t timeout_ms,
                                                std::int64_t max_timestamp_skew_ms,
                                                std::string* error_message);

}  // namespace ll::polymarket
