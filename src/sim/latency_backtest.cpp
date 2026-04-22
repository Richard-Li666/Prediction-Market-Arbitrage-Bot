#include "sim/latency_backtest.hpp"

#include <algorithm>
#include <cmath>

namespace ll::sim {

namespace {

bool is_polymarket_quote(const replay::LogEvent& e) {
  return e.source == "polymarket" && e.row.value("event_type", std::string{}) == "quote";
}

double poly_mid(const replay::LogEvent& e) {
  const auto& p = e.row.at("payload");
  const double bid = p.at("best_bid").get<double>();
  const double ask = p.at("best_ask").get<double>();
  return 0.5 * (bid + ask);
}

const replay::LogEvent* first_poly_at_or_after(const std::vector<replay::LogEvent>& m, int64_t t_ns) {
  auto it = std::lower_bound(m.begin(), m.end(), t_ns, [](const replay::LogEvent& e, int64_t t) {
    return e.sort_key_ns < t;
  });
  for (; it != m.end(); ++it) {
    if (is_polymarket_quote(*it)) {
      return &(*it);
    }
  }
  return nullptr;
}

bool deterministic_miss(std::size_t signal_index, double p) {
  if (p <= 0.0) {
    return false;
  }
  const unsigned x = static_cast<unsigned>(signal_index * 2654435761u);
  const double u = (x % 10'000) / 10'000.0;
  return u < p;
}

}  // namespace

std::vector<SimResultRow> run_latency_sweep(const std::vector<signals::Signal>& signals,
                                             const std::vector<replay::LogEvent>& merged_timeline,
                                             const SimConfig& cfg) {
  std::vector<SimResultRow> rows;
  rows.reserve(cfg.latencies_ms.size());

  const double fee_frac = (cfg.fee_bps / 10000.0) * 2.0;
  const double slip_frac = (cfg.slippage_bps / 10000.0);

  for (const int L : cfg.latencies_ms) {
    SimResultRow row;
    row.latency_ms = L;
    double pnl = 0.0;

    for (std::size_t si = 0; si < signals.size(); ++si) {
      const auto& s = signals[si];
      ++row.attempts;

      if (deterministic_miss(si, cfg.missed_fill_probability)) {
        continue;
      }

      const int64_t entry_ns = s.at_mono_ns + static_cast<int64_t>(L) * 1'000'000LL;

      const auto* e0 = first_poly_at_or_after(merged_timeline, entry_ns);
      if (!e0) {
        continue;
      }
      const int64_t exit_ns = e0->sort_key_ns + static_cast<int64_t>(cfg.exit_horizon_ms) * 1'000'000LL;
      const auto* e1 = first_poly_at_or_after(merged_timeline, exit_ns);
      if (!e1) {
        continue;
      }

      const double m0 = poly_mid(*e0);
      const double m1 = poly_mid(*e1);
      const double raw = (m1 - m0);
      const double adj = raw - fee_frac - slip_frac;
      pnl += adj;
      ++row.fills;
    }

    row.pnl_probability_points = pnl;
    rows.push_back(row);
  }

  return rows;
}

}  // namespace ll::sim
