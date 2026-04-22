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
};

/// Wall-clock 300s bucket + Gamma `/events?slug=...` confirmation (current, then +300, then −300).
/// Only fills `out` when an active, non-closed market with Up/Down outcomes is confirmed.
bool discover_active_btc_updown_5m_via_bucket(BtcFiveMinuteBucketDiscovery& out, std::string* error_message);

/// Single-slug Gamma confirmation (no ±300 fallback). Use to prefetch `btc-updown-5m-<epoch+300>`.
bool discover_btc_updown_5m_for_exact_slug(const std::string& slug,
                                           BtcFiveMinuteBucketDiscovery& out,
                                           std::string* error_message);

}  // namespace ll::polymarket
