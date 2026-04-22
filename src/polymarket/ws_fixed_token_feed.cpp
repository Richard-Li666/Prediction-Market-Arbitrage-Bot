#include "polymarket/ws_fixed_token_feed.hpp"

#include <cmath>
#include <limits>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "core/clock.hpp"
#include "telemetry/pipeline.hpp"

namespace ll::polymarket {

namespace {

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

bool parse_top_of_book_from_book(const nlohmann::json& j, double* bid, double* ask) {
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

}  // namespace

WsFixedTokenQuoteFeed::WsFixedTokenQuoteFeed(telemetry::Pipeline* tel) : tel_(tel) {}

WsFixedTokenQuoteFeed::~WsFixedTokenQuoteFeed() { stop(); }

void WsFixedTokenQuoteFeed::set_on_quote(QuoteFn fn) { on_quote_ = std::move(fn); }

void WsFixedTokenQuoteFeed::set_market_context(const std::string& event_slug,
                                               std::int64_t market_bucket_epoch,
                                               const std::string& outcome) {
  std::lock_guard<std::mutex> lk(sub_mu_);
  quote_event_slug_ = event_slug;
  quote_market_bucket_epoch_ = market_bucket_epoch;
  quote_outcome_ = outcome;
}

bool WsFixedTokenQuoteFeed::start(const std::string& token_id, std::string* error_message) {
  if (running_) {
    if (error_message) {
      *error_message = "WsFixedTokenQuoteFeed already running";
    }
    return false;
  }
  if (token_id.empty()) {
    if (error_message) {
      *error_message = "token_id is empty";
    }
    return false;
  }

  {
    std::lock_guard<std::mutex> lk(sub_mu_);
    token_id_ = token_id;
  }
  msg_count_ = 0;

  ws_.set_subscription_snapshot({token_id});
  ws_.set_on_open([this] { on_ws_open(); });
  ws_.set_on_closed([this] { on_ws_closed(); });
  ws_.set_on_message([this](const std::string& s) { on_ws_message(s); });

  if (!ws_.start()) {
    if (error_message) {
      *error_message = "MarketWsClient start failed";
    }
    std::lock_guard<std::mutex> lk(sub_mu_);
    token_id_.clear();
    return false;
  }

  running_ = true;
  return true;
}

void WsFixedTokenQuoteFeed::stop() {
  if (!running_) {
    return;
  }
  ws_.stop();
  running_ = false;
  {
    std::lock_guard<std::mutex> lk(sub_mu_);
    token_id_.clear();
    have_last_emitted_quote_ = false;
  }
}

void WsFixedTokenQuoteFeed::on_ws_open() {
  {
    std::lock_guard<std::mutex> lk(sub_mu_);
    have_last_emitted_quote_ = false;
  }
  if (tel_) {
    tel_->mark("poly_ws_open", ll::core::steady_ns());
  }
}

void WsFixedTokenQuoteFeed::on_ws_closed() {
  if (tel_) {
    tel_->mark("poly_ws_closed", ll::core::steady_ns());
  }
}

void WsFixedTokenQuoteFeed::on_ws_message(const std::string& s) {
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

  const std::string et = j.value("event_type", "");
  const std::string dedup_ts = j.contains("timestamp") ? j["timestamp"].dump() : "";

  if (et == "best_bid_ask") {
    const std::string aid = j.value("asset_id", "");
    std::string cur;
    {
      std::lock_guard<std::mutex> lk(sub_mu_);
      cur = token_id_;
    }
    if (aid != cur) {
      return;
    }
    const double bid = json_to_double(j.at("best_bid"));
    const double ask = json_to_double(j.at("best_ask"));
    emit_quote(aid, bid, ask, dedup_ts);
    return;
  }

  if (et == "book") {
    const std::string aid = j.value("asset_id", "");
    std::string cur;
    {
      std::lock_guard<std::mutex> lk(sub_mu_);
      cur = token_id_;
    }
    if (aid != cur) {
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
      std::string cur;
      {
        std::lock_guard<std::mutex> lk(sub_mu_);
        cur = token_id_;
      }
      if (aid != cur) {
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

void WsFixedTokenQuoteFeed::emit_quote(const std::string& asset_id, double bid, double ask,
                                       const std::string& dedup_ts) {
  (void)dedup_ts;
  PolymarketWsQuote q;
  {
    std::lock_guard<std::mutex> lk(sub_mu_);
    if (asset_id != token_id_) {
      return;
    }
    if (std::isnan(bid) || std::isnan(ask)) {
      return;
    }

    if (have_last_emitted_quote_ && bid == last_emitted_bid_ && ask == last_emitted_ask_) {
      return;
    }

    q.token_id = token_id_;
    q.event_slug = quote_event_slug_;
    q.market_bucket_epoch = quote_market_bucket_epoch_;
    q.outcome = quote_outcome_;
    q.best_bid = bid;
    q.best_ask = ask;
  }

  if (tel_) {
    tel_->mark("poly_quote_parsed", ll::core::steady_ns());
  }

  q.local_mono_ns = ll::core::steady_ns();
  q.local_wall_ms = ll::core::system_ms();

  if (on_quote_) {
    on_quote_(q);
  }

  {
    std::lock_guard<std::mutex> lk(sub_mu_);
    if (asset_id != token_id_) {
      return;
    }
    last_emitted_bid_ = bid;
    last_emitted_ask_ = ask;
    have_last_emitted_quote_ = true;
  }
}

bool WsFixedTokenQuoteFeed::switch_token(const std::string& new_token_id, std::string* error_message) {
  if (!running_) {
    if (error_message) {
      *error_message = "WsFixedTokenQuoteFeed not running";
    }
    return false;
  }
  if (new_token_id.empty()) {
    if (error_message) {
      *error_message = "new_token_id is empty";
    }
    return false;
  }

  std::string old_tok;
  {
    std::lock_guard<std::mutex> lk(sub_mu_);
    if (new_token_id == token_id_) {
      return true;
    }
    old_tok = token_id_;
  }

  if (!old_tok.empty()) {
    nlohmann::json un;
    un["operation"] = "unsubscribe";
    un["assets_ids"] = nlohmann::json::array({old_tok});
    ws_.send_json(un);
  }

  {
    std::lock_guard<std::mutex> lk(sub_mu_);
    token_id_ = new_token_id;
    have_last_emitted_quote_ = false;
  }

  ws_.set_subscription_snapshot({new_token_id});

  nlohmann::json sub;
  sub["operation"] = "subscribe";
  sub["assets_ids"] = nlohmann::json::array({new_token_id});
  sub["type"] = "market";
  sub["custom_feature_enabled"] = true;
  ws_.send_json(sub);

  return true;
}

}  // namespace ll::polymarket
