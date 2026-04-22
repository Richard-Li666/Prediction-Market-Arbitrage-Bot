#pragma once

#include <cstdint>
#include <string>

namespace ll::execution {

struct OrderIntent {
  int64_t mono_ns = 0;
  std::string side;
  double limit_price = 0.0;
  double qty = 0.0;
  std::string market_token_id;
};

class PaperExecutor {
 public:
  void record_intent(const OrderIntent& o);
};

class LiveExecutor {
 public:
  bool submit(const OrderIntent& o, std::string* error_message);
};

}  // namespace ll::execution
