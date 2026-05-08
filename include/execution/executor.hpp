#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ll::execution {

struct OrderIntent {
  int64_t mono_ns = 0;
  std::string side;
  double limit_price = 0.0;
  double qty = 0.0;
  std::string market_token_id;
  /// If true: daemon posts a FOK market order. BUY: `qty` is USD to spend; SELL: `qty` is shares.
  bool market_order = false;
};

class PaperExecutor {
 public:
  void record_intent(const OrderIntent& o);
};

class LiveExecutor {
 public:
  // If out_order_id is provided and submit succeeds, it will be filled.
  // If submit_latency_ns is provided: set to C++→daemon→C++ JSONL round-trip for the POST path (nanoseconds),
  // or -1 if BUILD_LIVE_TRADER is off / daemon did not complete a request.
  bool submit(const OrderIntent& o, std::string* error_message, std::string* out_order_id = nullptr,
              std::int64_t* submit_latency_ns = nullptr);

  /// CLOB conditional token balance in **shares** (for `token_id`), via py_clob balance-allowance API.
  /// Returns false if live trading is disabled, daemon fails, or balance is unavailable.
  bool query_conditional_balance(const std::string& token_id, double* out_shares, std::string* error_message,
                                  std::int64_t* query_latency_ns = nullptr);

  /// GARCH(1,1) volatility forecast via poly_daemon (`arch`; cmd `garch_forecast`).
  /// Available when CMake enables `LL_ENABLE_GARCH_DAEMON` (e.g. -DBUILD_DAEMON_GARCH=ON or BUILD_LIVE_TRADER).
  /// `resampled_mids`: evenly-spaced mid prices (C++ pre-resamples from bin_hist).
  /// `step_ms`: resample interval in ms.
  /// Returns annualized sigma on success.
  bool query_garch_sigma(const std::vector<double>& resampled_mids, std::int64_t step_ms,
                         double* out_sigma_annual, std::string* error_message,
                         std::int64_t* query_latency_ns = nullptr);
};

}  // namespace ll::execution
