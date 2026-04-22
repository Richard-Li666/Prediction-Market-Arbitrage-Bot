#pragma once

#include <string>

#include "core/types.hpp"

namespace ll::polymarket {

struct RestQuoteConfig {
  std::string token_id;
};

/// One-shot CLOB HTTP `book` fetch. Live tools use `PolyWsQuoteFeed` + websocket; this remains for ad-hoc tests.
bool fetch_order_book(const RestQuoteConfig& cfg, core::BookQuote& out, std::string* error_message);

}  // namespace ll::polymarket
