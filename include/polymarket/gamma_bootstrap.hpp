#pragma once

#include <string>

#include "polymarket/polymarket_types.hpp"

namespace ll::polymarket {

struct GammaBootstrapParams {
  /// Slug must start with this ASCII prefix, compared case-insensitively (default: BTC 5m Up/Down family).
  std::string slug_prefix{"btc-updown-5m"};
  bool require_up_down_outcomes{true};
};

/// One-shot HTTP fetch from Gamma API to locate an active market matching the slug prefix and Up token id.
bool gamma_discover_active_market(const GammaBootstrapParams& params,
                                  PolymarketMarketRef& out,
                                  std::string* error_message);

}  // namespace ll::polymarket
