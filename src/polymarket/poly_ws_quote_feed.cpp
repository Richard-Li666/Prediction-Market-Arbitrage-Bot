#include "polymarket/poly_ws_quote_feed.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "core/clock.hpp"
#include "telemetry/pipeline.hpp"

namespace ll::polymarket {

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

/// Same rule as Gamma bootstrap: slug must begin with prefix (ASCII), case-insensitive (strict family filter).
bool slug_has_prefix_ci(const std::string& slug, const std::string& prefix) {
  if (prefix.empty()) {
    return true;
  }
  const std::string ls = lower(slug);
  const std::string lp = lower(prefix);
  if (ls.size() < lp.size()) {
    return false;
  }
  return ls.compare(0, lp.size(), lp) == 0;
}

bool json_level_size_positive(const nlohmann::json& lvl) {
  if (!lvl.contains("size")) {
    return false;
  }
  const auto& sz = lvl.at("size");
  double v = 0;
  if (sz.is_number()) {
    v = sz.get<double>();
  } else if (sz.is_string()) {
    try {
      v = std::stod(sz.get<std::string>());
    } catch (...) {
      return false;
    }
  } else {
    return false;
  }
  return v > 1e-18;
}

double json_to_double(const nlohmann::json& j) {
  if (j.is_number()) {
    return j.get<double>();
  }
  if (j.is_string()) {
    try {
      return std::stod(j.get<std::string>());
    } catch (...) {
      return std::numeric_limits<double>::quiet_NaN();
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

std::vector<std::string> outcomes_from_new_market(const nlohmann::json& j) {
  std::vector<std::string> r;
  if (!j.contains("outcomes") || !j["outcomes"].is_array()) {
    return r;
  }
  for (const auto& x : j["outcomes"]) {
    if (x.is_string()) {
      r.push_back(x.get<std::string>());
    }
  }
  return r;
}

std::vector<std::string> tokens_from_new_market(const nlohmann::json& j) {
  std::vector<std::string> ids;
  const char* keys[] = {"clob_token_ids", "clobTokenIds", "assets_ids"};
  for (const char* k : keys) {
    if (!j.contains(k)) {
      continue;
    }
    const auto& t = j.at(k);
    if (t.is_array()) {
      for (const auto& x : t) {
        if (x.is_string()) {
          ids.push_back(x.get<std::string>());
        }
      }
      if (!ids.empty()) {
        return ids;
      }
    }
  }
  return ids;
}

std::size_t index_up(const std::vector<std::string>& outs) {
  for (std::size_t i = 0; i < outs.size(); ++i) {
    if (lower(outs[i]) == "up") {
      return i;
    }
  }
  return 0;
}

std::size_t index_down(const std::vector<std::string>& outs) {
  for (std::size_t i = 0; i < outs.size(); ++i) {
    if (lower(outs[i]) == "down") {
      return i;
    }
  }
  return outs.size() > 1 ? 1 : 0;
}

bool condition_same(const std::string& a, const std::string& b) {
  return lower(a) == lower(b);
}

std::uint64_t parse_ts_ms(const nlohmann::json& j) {
  if (!j.contains("timestamp")) {
    return 0;
  }
  const auto& t = j.at("timestamp");
  if (t.is_number_unsigned()) {
    return t.get<std::uint64_t>();
  }
  if (t.is_number_integer()) {
    const auto v = t.get<std::int64_t>();
    return v > 0 ? static_cast<std::uint64_t>(v) : 0;
  }
  if (t.is_string()) {
    try {
      return static_cast<std::uint64_t>(std::stoull(t.get<std::string>()));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

std::string json_scalar_id_string(const nlohmann::json& j, const char* key) {
  if (!j.contains(key)) {
    return {};
  }
  const auto& v = j.at(key);
  if (v.is_string()) {
    return v.get<std::string>();
  }
  if (v.is_number_unsigned()) {
    return std::to_string(v.get<std::uint64_t>());
  }
  if (v.is_number_integer()) {
    return std::to_string(v.get<std::int64_t>());
  }
  return {};
}

}  // namespace

PolyWsQuoteFeed::PolyWsQuoteFeed(telemetry::Pipeline* tel) : tel_(tel) {}

PolyWsQuoteFeed::~PolyWsQuoteFeed() { stop(); }

bool PolyWsQuoteFeed::guard_active_quote_asset(const std::string& asset_id) const {
  return !asset_id.empty() && asset_id == active_up_token_;
}

void PolyWsQuoteFeed::set_config(PolyWsQuoteFeedConfig cfg) { cfg_ = std::move(cfg); }

void PolyWsQuoteFeed::set_on_quote(QuoteFn fn) { on_quote_ = std::move(fn); }

void PolyWsQuoteFeed::set_on_rollover(RolloverFn fn) { on_rollover_ = std::move(fn); }

bool PolyWsQuoteFeed::start(std::string* error_message) {
  if (running_) {
    if (error_message) {
      *error_message = "PolyWsQuoteFeed already running";
    }
    return false;
  }

  msg_count_ = 0;

  PolymarketMarketRef boot;
  if (cfg_.initial_up_token_id.has_value()) {
    boot.up_token_id = *cfg_.initial_up_token_id;
    boot.down_token_id.clear();
    boot.active = true;
    active_ = boot;
    active_up_token_ = boot.up_token_id;
    active_announced_ts_ms_ = static_cast<std::uint64_t>(ll::core::system_ms());
  } else {
    std::string err;
    if (!gamma_discover_active_market(cfg_.gamma, boot, &err)) {
      if (error_message) {
        *error_message = err;
      }
      return false;
    }
    active_ = boot;
    active_up_token_ = boot.up_token_id;
    active_announced_ts_ms_ = static_cast<std::uint64_t>(ll::core::system_ms());
  }

  ws_.set_subscription_snapshot({active_up_token_});
  ws_.set_on_open([this] { on_ws_open(); });
  ws_.set_on_closed([this] { on_ws_closed(); });
  ws_.set_on_message([this](const std::string& s) { on_ws_message(s); });

  if (!ws_.start()) {
    if (error_message) {
      *error_message = "MarketWsClient start failed";
    }
    active_up_token_.clear();
    active_ = {};
    return false;
  }

  running_ = true;
  return true;
}

void PolyWsQuoteFeed::stop() {
  if (!running_) {
    return;
  }
  ws_.stop();
  running_ = false;
  active_up_token_.clear();
  active_ = {};
  active_announced_ts_ms_ = 0;
  last_quote_dedup_.clear();
}

void PolyWsQuoteFeed::on_ws_open() {
  // After reconnect, IXWebSocket replays only `set_subscription_snapshot` ids (see MarketWsClient). Clear dedup so
  // the first post-reconnect quote is not suppressed by a pre-drop identical tuple.
  last_quote_dedup_.clear();
  if (tel_) {
    tel_->mark("poly_ws_open", ll::core::steady_ns());
  }
}

void PolyWsQuoteFeed::on_ws_closed() {
  if (tel_) {
    tel_->mark("poly_ws_closed", ll::core::steady_ns());
  }
}

void PolyWsQuoteFeed::on_ws_message(const std::string& s) {
  ++msg_count_;
  if (tel_ && (msg_count_ == 1 || (msg_count_ % 512) == 0)) {
    tel_->mark("poly_ws_msg", ll::core::steady_ns());
  }

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(s);
  } catch (...) {
    return;
  }
  if (!j.is_object()) {
    return;
  }

  try_hydrate_from_message(j);

  const std::string et = j.value("event_type", "");
  if (et == "new_market") {
    maybe_handle_new_market(j);
    return;
  }

  if (active_up_token_.empty()) {
    return;
  }

  const std::string dedup_ts = j.contains("timestamp") ? j["timestamp"].dump() : "";

  if (et == "best_bid_ask") {
    const std::string aid = j.value("asset_id", "");
    if (!guard_active_quote_asset(aid)) {
      return;
    }
    const double bid = json_to_double(j.at("best_bid"));
    const double ask = json_to_double(j.at("best_ask"));
    emit_quote(aid, bid, ask, dedup_ts);
    return;
  }

  if (et == "book") {
    const std::string aid = j.value("asset_id", "");
    if (!guard_active_quote_asset(aid)) {
      return;
    }
    double bid = NAN;
    double ask = NAN;
    if (!parse_top_of_book_from_book(j, &bid, &ask)) {
      return;
    }
    emit_quote(aid, bid, ask, dedup_ts);
    return;
  }

  if (et == "price_change") {
    if (!j.contains("price_changes") || !j["price_changes"].is_array()) {
      return;
    }
    for (const auto& ch : j["price_changes"]) {
      if (!ch.is_object()) {
        continue;
      }
      const std::string aid = ch.value("asset_id", "");
      if (!guard_active_quote_asset(aid)) {
        continue;
      }
      if (!ch.contains("best_bid") || !ch.contains("best_ask")) {
        continue;
      }
      const double bid = json_to_double(ch.at("best_bid"));
      const double ask = json_to_double(ch.at("best_ask"));
      const std::string pts = ch.contains("timestamp") ? ch["timestamp"].dump() : dedup_ts;
      emit_quote(aid, bid, ask, pts.empty() ? dedup_ts : pts);
    }
  }
}

void PolyWsQuoteFeed::try_hydrate_from_message(const nlohmann::json& j) {
  if (!active_.condition_id.empty()) {
    return;
  }
  if (!j.contains("asset_id") || !j["asset_id"].is_string()) {
    return;
  }
  if (j["asset_id"].get<std::string>() != active_up_token_) {
    return;
  }
  if (!j.contains("market") || !j["market"].is_string()) {
    return;
  }
  active_.condition_id = j["market"].get<std::string>();
}

bool PolyWsQuoteFeed::parse_top_of_book_from_book(const nlohmann::json& j, double* bid,
                                                  double* ask) const {
  // Executable top-of-book: max bid price and min ask price among levels with strictly positive size (ordering-free).
  double best_bid = -1;
  double best_ask = -1;
  bool any_bid = false;
  bool any_ask = false;
  if (j.contains("bids") && j["bids"].is_array()) {
    for (const auto& lvl : j["bids"]) {
      if (!lvl.contains("price")) {
        continue;
      }
      if (!json_level_size_positive(lvl)) {
        continue;
      }
      const double p = json_to_double(lvl.at("price"));
      if (std::isnan(p)) {
        continue;
      }
      if (!any_bid || p > best_bid) {
        best_bid = p;
        any_bid = true;
      }
    }
  }
  if (j.contains("asks") && j["asks"].is_array()) {
    for (const auto& lvl : j["asks"]) {
      if (!lvl.contains("price")) {
        continue;
      }
      if (!json_level_size_positive(lvl)) {
        continue;
      }
      const double p = json_to_double(lvl.at("price"));
      if (std::isnan(p)) {
        continue;
      }
      if (!any_ask || p < best_ask) {
        best_ask = p;
        any_ask = true;
      }
    }
  }
  if (!any_bid || !any_ask) {
    return false;
  }
  *bid = best_bid;
  *ask = best_ask;
  return true;
}

bool PolyWsQuoteFeed::new_market_matches_family(const nlohmann::json& j) const {
  const std::string slug = j.value("slug", "");
  // Strict rollover family: prefix match (default slug starts with "btc-updown-5m") + binary Up/Down outcomes.
  if (!slug_has_prefix_ci(slug, cfg_.rollover_slug_prefix)) {
    return false;
  }
  const auto outs = outcomes_from_new_market(j);
  bool up = false;
  bool down = false;
  for (const auto& o : outs) {
    const auto lo = lower(o);
    if (lo == "up") {
      up = true;
    }
    if (lo == "down") {
      down = true;
    }
  }
  return up && down;
}

PolymarketMarketRef PolyWsQuoteFeed::market_ref_from_new_market(const nlohmann::json& j) const {
  PolymarketMarketRef r;
  r.market_id = json_scalar_id_string(j, "id");
  r.condition_id = j.value("market", std::string{});
  if (r.condition_id.empty()) {
    r.condition_id = j.value("condition_id", std::string{});
  }
  r.slug = j.value("slug", std::string{});
  r.active = j.value("active", true);
  const auto outs = outcomes_from_new_market(j);
  const auto toks = tokens_from_new_market(j);
  if (!toks.empty() && outs.size() == toks.size()) {
    r.up_token_id = toks.at(index_up(outs));
    r.down_token_id = toks.at(index_down(outs));
  }
  return r;
}

bool PolyWsQuoteFeed::maybe_handle_new_market(const nlohmann::json& j) {
  if (!new_market_matches_family(j)) {
    return false;
  }
  PolymarketMarketRef next = market_ref_from_new_market(j);
  if (next.up_token_id.empty()) {
    return false;
  }
  const std::uint64_t ts = parse_ts_ms(j);
  if (!active_.condition_id.empty() && !next.condition_id.empty() &&
      condition_same(active_.condition_id, next.condition_id)) {
    return false;
  }
  switch_to_market(next, ts);
  return true;
}

void PolyWsQuoteFeed::switch_to_market(const PolymarketMarketRef& next, std::uint64_t announce_ts_ms) {
  // Snapshot authoritative pre-switch state for rollover logging (tokens + ids) before mutating active_*.
  PolymarketMarketRef old = active_;
  const std::string old_tok = active_up_token_;

  if (!old_tok.empty() && old_tok != next.up_token_id) {
    nlohmann::json un;
    un["operation"] = "unsubscribe";
    un["assets_ids"] = nlohmann::json::array({old_tok});
    ws_.send_json(un);
  }

  active_ = next;
  active_up_token_ = next.up_token_id;
  if (announce_ts_ms > 0) {
    active_announced_ts_ms_ = announce_ts_ms;
  } else {
    active_announced_ts_ms_ = static_cast<std::uint64_t>(ll::core::system_ms());
  }

  // Reconnect path only replays this vector — keep exactly one id (the post-rollover Up token).
  ws_.set_subscription_snapshot({active_up_token_});

  nlohmann::json sub;
  sub["operation"] = "subscribe";
  sub["assets_ids"] = nlohmann::json::array({active_up_token_});
  sub["type"] = "market";
  sub["custom_feature_enabled"] = true;
  ws_.send_json(sub);

  last_quote_dedup_.clear();

  if (tel_) {
    tel_->mark("poly_rollover", ll::core::steady_ns());
  }
  if (on_rollover_) {
    on_rollover_(old, next);
  }
}

void PolyWsQuoteFeed::emit_quote(const std::string& quote_asset_id, double bid, double ask,
                                 const std::string& dedup_ts) {
  // Final stale guard: unsubscribed assets may still deliver in-flight frames after rollover.
  if (!guard_active_quote_asset(quote_asset_id)) {
    return;
  }
  if (std::isnan(bid) || std::isnan(ask)) {
    return;
  }
  const std::string k =
      quote_asset_id + "|" + std::to_string(bid) + "|" + std::to_string(ask) + "|" + dedup_ts;
  if (k == last_quote_dedup_) {
    return;
  }
  last_quote_dedup_ = k;

  if (tel_) {
    tel_->mark("poly_quote_parsed", ll::core::steady_ns());
  }

  PolymarketQuoteTick t;
  t.token_id = quote_asset_id;
  t.outcome = "Up";
  t.market_slug = active_.slug;
  t.market_id = active_.market_id.empty() ? active_.condition_id : active_.market_id;
  t.best_bid = bid;
  t.best_ask = ask;
  t.local_mono_ns = ll::core::steady_ns();
  t.local_wall_ms = ll::core::system_ms();

  if (on_quote_) {
    on_quote_(t);
  }
}

}  // namespace ll::polymarket
