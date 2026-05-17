#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ll::execution {

struct OrderIntent {
  int64_t mono_ns = 0;
  std::string side;
  double limit_price = 0.0;
  double qty = 0.0;
  std::string market_token_id;
  /// If true: daemon posts a market order. BUY: `qty` is USD to spend; SELL: `qty` is shares.
  bool market_order = false;
  /// Market order type passed to poly_daemon (`FAK`, `FOK`, …). Empty → `POLY_MARKET_ORDER_TYPE` env.
  std::string market_order_type;
  /// Worst-price limit for market orders (slippage floor). <=0 → daemon/SDK default.
  double market_worst_price = 0.0;
};

class PaperExecutor {
 public:
  void record_intent(const OrderIntent& o);
};

class LiveExecutor {
 public:
  /// If non-empty, each daemon JSONL request/response appends one row (see `emit_poly_daemon_traffic` in .cpp).
  void set_poly_daemon_traffic_log(std::function<void(const nlohmann::json&)> sink) {
    poly_daemon_traffic_sink_ = std::move(sink);
  }

  // If out_order_id is provided and submit succeeds, it will be filled.
  // If submit_latency_ns is provided: set to C++→daemon→C++ JSONL round-trip for the POST path (nanoseconds),
  // or -1 if BUILD_LIVE_TRADER is off / daemon did not complete a request.
  bool submit(const OrderIntent& o, std::string* error_message, std::string* out_order_id = nullptr,
              std::int64_t* submit_latency_ns = nullptr, nlohmann::json* out_submit_resp = nullptr);

  /// CLOB conditional token balance in **shares** (for `token_id`), via py_clob balance-allowance API.
  /// Returns false if live trading is disabled, daemon fails, or balance is unavailable.
  bool query_conditional_balance(const std::string& token_id, double* out_shares, std::string* error_message,
                                  std::int64_t* query_latency_ns = nullptr);

  /// CLOB order by id (`get_order`). Returns false if missing or daemon error.
  bool query_order(const std::string& order_id, nlohmann::json* out_order, std::string* error_message,
                   std::int64_t* query_latency_ns = nullptr);

  /// GARCH(1,1) volatility forecast via poly_daemon (`arch`; cmd `garch_forecast`).
  /// Available when CMake enables `LL_ENABLE_GARCH_DAEMON` (e.g. -DBUILD_DAEMON_GARCH=ON or BUILD_LIVE_TRADER).
  /// `resampled_mids`: evenly-spaced mid prices (C++ pre-resamples from bin_hist).
  /// `step_ms`: resample interval in ms.
  /// Returns annualized sigma on success.
  bool query_garch_sigma(const std::vector<double>& resampled_mids, std::int64_t step_ms,
                         double* out_sigma_annual, std::string* error_message,
                         std::int64_t* query_latency_ns = nullptr);

 private:
  std::function<void(const nlohmann::json&)> poly_daemon_traffic_sink_;
  void emit_poly_daemon_traffic(const nlohmann::json& request_log, bool transport_ok,
                                const std::string& transport_err, const std::string& response_line,
                                std::int64_t latency_ns);
};

}  // namespace ll::execution
