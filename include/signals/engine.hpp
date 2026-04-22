#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "replay/timeline_merge.hpp"

namespace ll::signals {

struct Signal {
  int64_t at_mono_ns = 0;
  double price = 0;
  std::string symbol;
};

struct SignalConfig {
  double move_bps = 5.0;
};

std::vector<Signal> detect_binance_moves(const std::vector<replay::LogEvent>& events,
                                          const SignalConfig& cfg);

}  // namespace ll::signals
