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

void LiveExecutor::emit_poly_daemon_traffic(const nlohmann::json& request_log, bool transport_ok,
                                            const std::string& transport_err,
                                            const std::string& response_line, std::int64_t latency_ns) {
  if (!poly_daemon_traffic_sink_) {
    return;
  }
  nlohmann::json row;
  row["schema_version"] = 1;
  row["channel"] = "poly_daemon";
  row["local_ts_mono_ns"] = ll::core::steady_ns();
  row["local_ts_wall_ms"] = ll::core::system_ms();
  row["latency_ns"] = latency_ns;
  row["transport_ok"] = transport_ok;
  if (!transport_err.empty()) {
    row["transport_error"] = transport_err;
  }
  row["request"] = request_log;
  row["response_raw"] = response_line;
  bool parse_ok = false;
  if (!response_line.empty()) {
    try {
      row["response"] = nlohmann::json::parse(response_line);
      parse_ok = true;
    } catch (...) {
      row["response"] = nullptr;
    }
  } else {
    row["response"] = nullptr;
  }
  row["response_parse_ok"] = parse_ok;
  poly_daemon_traffic_sink_(row);
}

bool LiveExecutor::submit(const OrderIntent& o, std::string* error_message, std::string* out_order_id,
                          std::int64_t* submit_latency_ns, nlohmann::json* out_submit_resp) {
#ifdef LL_ENABLE_LIVE_TRADER
  if (submit_latency_ns) {
    *submit_latency_ns = -1;
  }
  if (out_submit_resp) {
    *out_submit_resp = nlohmann::json::object();
  }

  nlohmann::json req;
  if (o.market_order) {
    req["cmd"] = "place_market_order";
    req["token_id"] = o.market_token_id;
    req["side"] = o.side;
    const std::string mot =
        !o.market_order_type.empty() ? o.market_order_type : getenv_str("POLY_MARKET_ORDER_TYPE");
    req["order_type"] = mot.empty() ? "FAK" : mot;
    req["amount"] = o.qty;
    if (o.market_worst_price > 0.0 && std::isfinite(o.market_worst_price)) {
      req["price"] = o.market_worst_price;
    }
  } else {
    req["cmd"] = "place_order";
    req["token_id"] = o.market_token_id;
    req["side"] = o.side;
    req["price"] = o.limit_price;
    req["size"] = o.qty;
    req["order_type"] = "GTC";
  }

  const std::string cmd = getenv_str("POLY_DAEMON_CMD");
  std::string err;
  if (!daemon().start(cmd, &err)) {
    emit_poly_daemon_traffic(req, false, "start daemon failed: " + err, "", 0);
    if (error_message) *error_message = "start daemon failed: " + err;
    return false;
  }

  const std::int64_t t0 = ll::core::steady_ns();
  std::string resp_line;
  const bool io_ok = daemon().request_response_jsonl(req.dump(), &resp_line, &err);
  const std::int64_t dt_ns = ll::core::steady_ns() - t0;
  if (submit_latency_ns) {
    *submit_latency_ns = dt_ns;
  }
  emit_poly_daemon_traffic(req, io_ok, io_ok ? "" : err, resp_line, dt_ns);
  if (!io_ok) {
    if (error_message) *error_message = "daemon request failed: " + err;
    return false;
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

  std::string order_id;
  if (resp.contains("order_id") && resp["order_id"].is_string()) {
    order_id = resp["order_id"].get<std::string>();
  }
  if (order_id.empty() && resp.contains("resp") && resp["resp"].is_object()) {
    auto& r = resp["resp"];
    if (r.contains("orderID") && r["orderID"].is_string()) order_id = r["orderID"].get<std::string>();
    if (order_id.empty() && r.contains("order_id") && r["order_id"].is_string()) order_id = r["order_id"].get<std::string>();
    if (order_id.empty() && r.contains("orderId") && r["orderId"].is_string()) order_id = r["orderId"].get<std::string>();
  }

  if (out_order_id) *out_order_id = order_id;
  if (out_submit_resp) *out_submit_resp = std::move(resp);
  if (error_message) *error_message = order_id.empty() ? resp_line : "";
  return true;
#else
  if (submit_latency_ns) {
    *submit_latency_ns = -1;
  }
  if (out_submit_resp) {
    *out_submit_resp = nlohmann::json::object();
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

  nlohmann::json req;
  req["cmd"] = "get_conditional_balance";
  req["token_id"] = token_id;

  const std::string cmd = getenv_str("POLY_DAEMON_CMD");
  std::string err;
  if (!daemon().start(cmd, &err)) {
    emit_poly_daemon_traffic(req, false, "start daemon failed: " + err, "", 0);
    if (error_message) *error_message = "start daemon failed: " + err;
    return false;
  }

  const std::int64_t t0 = ll::core::steady_ns();
  std::string resp_line;
  const bool io_ok = daemon().request_response_jsonl(req.dump(), &resp_line, &err);
  const std::int64_t dt_ns = ll::core::steady_ns() - t0;
  if (query_latency_ns) {
    *query_latency_ns = dt_ns;
  }
  emit_poly_daemon_traffic(req, io_ok, io_ok ? "" : err, resp_line, dt_ns);
  if (!io_ok) {
    if (error_message) *error_message = "daemon request failed: " + err;
    return false;
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

bool LiveExecutor::query_order(const std::string& order_id, nlohmann::json* out_order,
                               std::string* error_message, std::int64_t* query_latency_ns) {
#ifdef LL_ENABLE_LIVE_TRADER
  if (query_latency_ns) {
    *query_latency_ns = -1;
  }
  if (out_order) {
    *out_order = nlohmann::json::object();
  }

  nlohmann::json req;
  req["cmd"] = "get_order";
  req["order_id"] = order_id;

  const std::string cmd = getenv_str("POLY_DAEMON_CMD");
  std::string err;
  if (!daemon().start(cmd, &err)) {
    emit_poly_daemon_traffic(req, false, "start daemon failed: " + err, "", 0);
    if (error_message) *error_message = "start daemon failed: " + err;
    return false;
  }

  const std::int64_t t0 = ll::core::steady_ns();
  std::string resp_line;
  const bool io_ok = daemon().request_response_jsonl(req.dump(), &resp_line, &err);
  const std::int64_t dt_ns = ll::core::steady_ns() - t0;
  if (query_latency_ns) {
    *query_latency_ns = dt_ns;
  }
  emit_poly_daemon_traffic(req, io_ok, io_ok ? "" : err, resp_line, dt_ns);
  if (!io_ok) {
    if (error_message) *error_message = "daemon request failed: " + err;
    return false;
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

  nlohmann::json order = nlohmann::json::object();
  if (resp.contains("order") && resp["order"].is_object()) {
    order = resp["order"];
  } else if (resp.contains("resp") && resp["resp"].is_object()) {
    order = resp["resp"];
  } else {
    if (error_message) *error_message = "no order object in response: " + resp_line;
    return false;
  }

  if (out_order) {
    *out_order = std::move(order);
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

  nlohmann::json req;
  req["cmd"] = "garch_forecast";
  req["mids"] = resampled_mids;
  req["step_ms"] = step_ms;
  nlohmann::json req_log = req;
  if (req_log.contains("mids") && req_log["mids"].is_array()) {
    req_log["n_mids"] = req_log["mids"].size();
    req_log.erase("mids");
  }

  const std::string cmd = getenv_str("POLY_DAEMON_CMD");
  std::string err;
  if (!daemon().start(cmd, &err)) {
    emit_poly_daemon_traffic(req_log, false, "start daemon failed: " + err, "", 0);
    if (error_message) *error_message = "start daemon failed: " + err;
    return false;
  }

  const std::int64_t t0 = ll::core::steady_ns();
  std::string resp_line;
  const bool io_ok = daemon().request_response_jsonl(req.dump(), &resp_line, &err);
  const std::int64_t dt_ns = ll::core::steady_ns() - t0;
  if (query_latency_ns) *query_latency_ns = dt_ns;
  emit_poly_daemon_traffic(req_log, io_ok, io_ok ? "" : err, resp_line, dt_ns);
  if (!io_ok) {
    if (error_message) *error_message = "daemon request failed: " + err;
    return false;
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
