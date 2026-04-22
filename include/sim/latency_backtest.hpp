#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "replay/timeline_merge.hpp"
#include "signals/engine.hpp"

namespace ll::sim {

struct SimConfig {
  std::vector<int> latencies_ms{25, 50, 100, 200};
  double fee_bps = 10.0;
  double slippage_bps = 5.0;
  double missed_fill_probability = 0.0;
  int exit_horizon_ms = 500;
};

struct SimResultRow {
  int latency_ms = 0;
  int attempts = 0;
  int fills = 0;
  double pnl_probability_points = 0.0;
};

std::vector<SimResultRow> run_latency_sweep(const std::vector<signals::Signal>& signals,
                                             const std::vector<replay::LogEvent>& merged_timeline,
                                             const SimConfig& cfg);

}  // namespace ll::sim
