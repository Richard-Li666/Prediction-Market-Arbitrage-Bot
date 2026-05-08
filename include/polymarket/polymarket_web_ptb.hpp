#pragma once

#include <string>

namespace ll::polymarket {

struct BtcFiveMinuteBucketDiscovery;

/// Fetch `https://polymarket.com/event/<slug>` and read Next.js `__NEXT_DATA__` →
/// matching `event.slug` → `eventMetadata.priceToBeat` (same hydration the web UI uses).
/// Fragile if Polymarket changes HTML layout; intended for optional rollover alignment only.
bool fill_strike_from_polymarket_web_event_page(BtcFiveMinuteBucketDiscovery& disc,
                                                std::string* error_message);

}  // namespace ll::polymarket
