#include "execution/executor.hpp"

#include <cstdlib>
#include <nlohmann/json.hpp>

#include "core/clock.hpp"
#include "execution/poly_daemon_client.hpp"

namespace ll::execution {

namespace {
PolyDaemonClient& daemon() {
  static PolyDaemonClient d;
  return d;
}

static std::string getenv_str(const char* k) {
  if (const char* v = std::getenv(k)) return std::string(v);
  return {};
}

}  // namespace

bool LiveExecutor::submit(const OrderIntent& o, std::string* error_message, std::string* out_order_id,
                            std::int64_t* submit_latency_ns) {
#ifdef LL_ENABLE_LIVE_TRADER
  if (submit_latency_ns) {
    *submit_latency_ns = -1;
  }
  const std::string cmd = getenv_str("POLY_DAEMON_CMD");
  std::string err;
  if (!daemon().start(cmd, &err)) {
    if (error_message) *error_message = "start daemon failed: " + err;
    return false;
  }

  nlohmann::json req;
  if (o.market_order) {
    req["cmd"] = "place_market_order";
    req["token_id"] = o.market_token_id;
    req["side"] = o.side;
    // Market order type: default to IOC to avoid frequent FOK kills on thin books.
    const std::string mot = getenv_str("POLY_MARKET_ORDER_TYPE");
    req["order_type"] = mot.empty() ? "IOC" : mot;
    req["amount"] = o.qty;
  } else {
    req["cmd"] = "place_order";
    req["token_id"] = o.market_token_id;
    req["side"] = o.side;
    req["price"] = o.limit_price;
    req["size"] = o.qty;
    req["order_type"] = "GTC";
  }

  const std::int64_t t0 = ll::core::steady_ns();
  std::string resp_line;
  if (!daemon().request_response_jsonl(req.dump(), &resp_line, &err)) {
    if (submit_latency_ns) {
      *submit_latency_ns = ll::core::steady_ns() - t0;
    }
    if (error_message) *error_message = "daemon request failed: " + err;
    return false;
  }
  const std::int64_t dt_ns = ll::core::steady_ns() - t0;
  if (submit_latency_ns) {
    *submit_latency_ns = dt_ns;
  }

  nlohmann::json resp;
  try {
    resp = nlohmann::json::parse(resp_line);
  } catch (...) {
    if (error_message) *error_message = "daemon returned non-JSON: " + resp_line;
    return false;
  }

  if (!resp.value("ok", false)) {
    if (error_message) *error_message = resp.value("error", "daemon error") + " raw=" + resp_line;
    return false;
  }

  // Try to extract order id from common places.
  std::string order_id;
  if (resp.contains("order_id") && resp["order_id"].is_string()) {
    order_id = resp["order_id"].get<std::string>();
  } else if (resp.contains("resp") && resp["resp"].is_object()) {
    auto& r = resp["resp"];
    if (r.contains("orderID") && r["orderID"].is_string()) order_id = r["orderID"].get<std::string>();
    if (order_id.empty() && r.contains("order_id") && r["order_id"].is_string()) order_id = r["order_id"].get<std::string>();
    if (order_id.empty() && r.contains("orderId") && r["orderId"].is_string()) order_id = r["orderId"].get<std::string>();
  }

  if (out_order_id) *out_order_id = order_id;
  // Caller prints order_id; avoid duplicating it in the message line.
  if (error_message) *error_message = order_id.empty() ? resp_line : "";
  return true;
#else
  if (submit_latency_ns) {
    *submit_latency_ns = -1;
  }
  if (error_message) {
    *error_message = "Live trading disabled: configure CMake with -DBUILD_LIVE_TRADER=ON (still a stub).";
  }
  return false;
#endif
}

bool LiveExecutor::query_conditional_balance(const std::string& token_id, double* out_shares,
                                              std::string* error_message, std::int64_t* query_latency_ns) {
#ifdef LL_ENABLE_LIVE_TRADER
  if (query_latency_ns) {
    *query_latency_ns = -1;
  }
  if (out_shares) {
    *out_shares = 0.0;
  }

  const std::string cmd = getenv_str("POLY_DAEMON_CMD");
  std::string err;
  if (!daemon().start(cmd, &err)) {
    if (error_message) *error_message = "start daemon failed: " + err;
    return false;
  }

  nlohmann::json req;
  req["cmd"] = "get_conditional_balance";
  req["token_id"] = token_id;

  const std::int64_t t0 = ll::core::steady_ns();
  std::string resp_line;
  if (!daemon().request_response_jsonl(req.dump(), &resp_line, &err)) {
    if (query_latency_ns) {
      *query_latency_ns = ll::core::steady_ns() - t0;
    }
    if (error_message) *error_message = "daemon request failed: " + err;
    return false;
  }
  const std::int64_t dt_ns = ll::core::steady_ns() - t0;
  if (query_latency_ns) {
    *query_latency_ns = dt_ns;
  }

  nlohmann::json resp;
  try {
    resp = nlohmann::json::parse(resp_line);
  } catch (...) {
    if (error_message) *error_message = "daemon returned non-JSON: " + resp_line;
    return false;
  }

  if (!resp.value("ok", false)) {
    if (error_message) {
      *error_message = resp.value("error", "daemon error") + " raw=" + resp_line;
    }
    return false;
  }

  double bal = 0.0;
  if (resp.contains("balance")) {
    const auto& b = resp["balance"];
    if (b.is_number()) {
      bal = b.get<double>();
    } else if (b.is_string()) {
      try {
        bal = std::stod(b.get<std::string>());
      } catch (...) {
        if (error_message) *error_message = "balance not numeric: " + resp_line;
        return false;
      }
    } else {
      if (error_message) *error_message = "balance missing or invalid type: " + resp_line;
      return false;
    }
  } else {
    if (error_message) *error_message = "no balance field: " + resp_line;
    return false;
  }

  if (out_shares) {
    *out_shares = bal;
  }
  if (error_message) {
    error_message->clear();
  }
  return true;
#else
  if (query_latency_ns) {
    *query_latency_ns = -1;
  }
  if (error_message) {
    *error_message = "Live trading disabled: configure CMake with -DBUILD_LIVE_TRADER=ON.";
  }
  return false;
#endif
}

bool LiveExecutor::query_garch_sigma(const std::vector<double>& resampled_mids, std::int64_t step_ms,
                                     double* out_sigma_annual, std::string* error_message,
                                     std::int64_t* query_latency_ns) {
#ifdef LL_ENABLE_GARCH_DAEMON
  if (query_latency_ns) *query_latency_ns = -1;
  if (out_sigma_annual) *out_sigma_annual = 0.0;

  const std::string cmd = getenv_str("POLY_DAEMON_CMD");
  std::string err;
  if (!daemon().start(cmd, &err)) {
    if (error_message) *error_message = "start daemon failed: " + err;
    return false;
  }

  nlohmann::json req;
  req["cmd"] = "garch_forecast";
  req["mids"] = resampled_mids;
  req["step_ms"] = step_ms;

  const std::int64_t t0 = ll::core::steady_ns();
  std::string resp_line;
  if (!daemon().request_response_jsonl(req.dump(), &resp_line, &err)) {
    if (query_latency_ns) *query_latency_ns = ll::core::steady_ns() - t0;
    if (error_message) *error_message = "daemon request failed: " + err;
    return false;
  }
  if (query_latency_ns) *query_latency_ns = ll::core::steady_ns() - t0;

  nlohmann::json resp;
  try {
    resp = nlohmann::json::parse(resp_line);
  } catch (...) {
    if (error_message) *error_message = "daemon returned non-JSON: " + resp_line;
    return false;
  }

  if (!resp.value("ok", false)) {
    if (error_message) *error_message = resp.value("error", "daemon error") + " raw=" + resp_line;
    return false;
  }

  if (!resp.contains("sigma_annual") || !resp["sigma_annual"].is_number()) {
    if (error_message) *error_message = "no sigma_annual in response: " + resp_line;
    return false;
  }

  double sig = resp["sigma_annual"].get<double>();
  if (out_sigma_annual) *out_sigma_annual = sig;
  if (error_message) error_message->clear();
  return true;
#else
  if (query_latency_ns) *query_latency_ns = -1;
  if (error_message) {
    *error_message =
        "GARCH daemon client not compiled: use cmake -DBUILD_DAEMON_GARCH=ON (paper) "
        "or -DBUILD_LIVE_TRADER=ON.";
  }
  return false;
#endif
}

}  // namespace ll::execution
