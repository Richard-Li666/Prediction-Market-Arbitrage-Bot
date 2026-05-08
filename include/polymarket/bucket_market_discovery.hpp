#pragma once

#include <cstdint>
#include <string>

namespace ll::polymarket {

/// Result of Gamma-confirmed discovery for the rolling BTC 5m Up/Down market (`btc-updown-5m-<epoch>`).
struct BtcFiveMinuteBucketDiscovery {
  /// Slug that Gamma returned and passed validation (one of current / ±300s buckets).
  std::string confirmed_slug;
  /// Parsed from `btc-updown-5m-<epoch>` suffix when possible; else -1.
  std::int64_t bucket_epoch_seconds{-1};
  std::string up_token_id;
  std::string down_token_id;
  std::string condition_id;
  std::string market_numeric_id;
  /// Strike aligned with Polymarket settlement: Gamma `eventMetadata.priceToBeat` when present,
  /// else approximate via Polymarket RTDS `crypto_prices_chainlink` (`btc/usd`) near `event_start_wall_ms`.
  double price_to_beat{0.0};
  /// When false, Gamma omitted strike metadata (common on current API).
  bool gamma_has_price_to_beat{true};
  /// True when `price_to_beat` was filled from Polymarket RTDS Chainlink stream (`btc/usd`).
  bool strike_from_polymarket_rtds_chainlink{false};
  /// True when filled from `polymarket.com/event/<slug>` Next.js `__NEXT_DATA__` (optional; e.g. rollover HTTP).
  bool strike_from_polymarket_web_event_page{false};
  /// Unix epoch **milliseconds** at bucket open from Gamma (`markets[].eventStartTime` or event `startTime`),
  /// else `bucket_epoch_seconds * 1000`. Used to align RTDS Chainlink ticks.
  std::int64_t event_start_wall_ms{-1};
};

/// Wall-clock 300s bucket + Gamma `/events?slug=...` (current, then +300, then −300).
/// Only accepts a market whose epoch satisfies `now ∈ [epoch, epoch+300)` so we never attach to the
/// previous bucket while Gamma still reports it active.
bool discover_active_btc_updown_5m_via_bucket(BtcFiveMinuteBucketDiscovery& out, std::string* error_message);

/// Single-slug Gamma confirmation (no ±300 fallback). Use to prefetch `btc-updown-5m-<epoch+300>`.
bool discover_btc_updown_5m_for_exact_slug(const std::string& slug,
                                           BtcFiveMinuteBucketDiscovery& out,
                                           std::string* error_message);

}  // namespace ll::polymarket
