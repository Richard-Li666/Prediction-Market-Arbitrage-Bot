#pragma once

#include "binance/stream_client.hpp"

namespace ll::binance {

/// If set, `LL_BINANCE_WS_HOST` and `LL_BINANCE_WS_PORT` override `cfg` after struct defaults.
/// Call before parsing CLI `--host` / `--port` so command line wins.
void apply_stream_env_overrides(StreamClientConfig& cfg);

}  // namespace ll::binance
