#include "execution/executor.hpp"

namespace ll::execution {

bool LiveExecutor::submit(const OrderIntent& o, std::string* error_message) {
  (void)o;
#ifdef LL_ENABLE_LIVE_TRADER
  if (error_message) {
    *error_message =
        "Live trading is compiled in, but no broker integration is wired yet (intentionally empty).";
  }
  return false;
#else
  if (error_message) {
    *error_message = "Live trading disabled: configure CMake with -DBUILD_LIVE_TRADER=ON (still a stub).";
  }
  return false;
#endif
}

}  // namespace ll::execution
