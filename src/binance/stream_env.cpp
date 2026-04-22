#include "binance/stream_env.hpp"

#include <cstdlib>

namespace ll::binance {

void apply_stream_env_overrides(StreamClientConfig& cfg) {
  if (const char* h = std::getenv("LL_BINANCE_WS_HOST")) {
    if (h[0] != '\0') {
      cfg.ws_host = h;
    }
  }
  if (const char* p = std::getenv("LL_BINANCE_WS_PORT")) {
    if (p[0] != '\0') {
      cfg.ws_port = std::atoi(p);
    }
  }
}

}  // namespace ll::binance
