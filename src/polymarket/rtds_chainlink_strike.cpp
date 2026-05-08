#include "polymarket/rtds_chainlink_strike.hpp"

#include "polymarket/bucket_market_discovery.hpp"
#include "polymarket/rtds_chainlink_feed.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace ll::polymarket {

namespace {

void log_rtds_strike_debug(const BtcFiveMinuteBucketDiscovery& disc, std::int64_t target_ms,
                           std::int64_t payload_ts_ms, double px, const char* pick, bool immediate) {
  const std::int64_t dt_ms = payload_ts_ms - target_ms;
  std::cerr << "[rtds_chainlink] strike_debug";
  if (!disc.confirmed_slug.empty()) {
    std::cerr << " slug=" << disc.confirmed_slug;
  }
  if (disc.bucket_epoch_seconds >= 0) {
    const std::int64_t boundary_from_slug_ms = disc.bucket_epoch_seconds * 1000;
    std::cerr << " bucket_epoch_s=" << disc.bucket_epoch_seconds << " boundary_from_slug_ms="
              << boundary_from_slug_ms << " target_eq_boundary=" << (target_ms == boundary_from_slug_ms ? 1 : 0);
  }
  std::cerr << " target_wall_ms=" << target_ms << " payload_ts_ms=" << payload_ts_ms << " dt_ms=" << dt_ms
            << " pick=" << pick << " immediate=" << (immediate ? 1 : 0) << " px=" << px
            << " rtds_feed=persistent\n";
}

}  // namespace

bool fill_strike_from_polymarket_rtds_chainlink(BtcFiveMinuteBucketDiscovery& disc,
                                                std::int64_t timeout_ms,
                                                std::int64_t max_timestamp_skew_ms,
                                                std::string* error_message) {
  if (disc.gamma_has_price_to_beat && std::isfinite(disc.price_to_beat) && disc.price_to_beat > 0.0) {
    return true;
  }
  std::int64_t target_ms = disc.event_start_wall_ms;
  if (target_ms < 0 && disc.bucket_epoch_seconds >= 0) {
    target_ms = disc.bucket_epoch_seconds * 1000;
  }
  if (target_ms < 0) {
    if (error_message) {
      *error_message = "RTDS strike needs event_start_wall_ms or bucket_epoch from Gamma";
    }
    return false;
  }
  if (timeout_ms < 500) {
    timeout_ms = 500;
  }
  if (max_timestamp_skew_ms < 1000) {
    max_timestamp_skew_ms = 1000;
  }

  RtdsChainlinkFeed::instance().ensure_started();

  double px = 0.0;
  std::int64_t payload_ts = 0;
  const char* pick = "";
  bool immediate = false;
  if (!RtdsChainlinkFeed::instance().wait_strike(target_ms, max_timestamp_skew_ms, timeout_ms, &px, &payload_ts, &pick,
                                                 &immediate, error_message)) {
    return false;
  }
  disc.price_to_beat = px;
  disc.strike_from_polymarket_rtds_chainlink = true;
  log_rtds_strike_debug(disc, target_ms, payload_ts, px, pick, immediate);
  return true;
}

}  // namespace ll::polymarket
