#include "signals/engine.hpp"

#include <cmath>

namespace ll::signals {

namespace {

bool is_binance_trade(const replay::LogEvent& e) {
  return e.source == "binance" && e.row.value("event_type", std::string{}) == "trade";
}

double trade_price(const replay::LogEvent& e) {
  return e.row.at("payload").at("price").get<double>();
}

}  // namespace

std::vector<Signal> detect_binance_moves(const std::vector<replay::LogEvent>& events,
                                         const SignalConfig& cfg) {
  std::vector<Signal> out;
  double last_px = 0.0;

  for (const auto& ev : events) {
    if (!is_binance_trade(ev)) {
      continue;
    }
    const double px = trade_price(ev);
    if (last_px <= 0.0) {
      last_px = px;
      continue;
    }
    const double bps = std::abs(px - last_px) / last_px * 10000.0;
    if (bps >= cfg.move_bps) {
      Signal s;
      s.at_mono_ns = ev.sort_key_ns;
      s.price = px;
      s.symbol = ev.row.value("symbol", std::string{});
      out.push_back(s);
    }
    last_px = px;
  }
  return out;
}

}  // namespace ll::signals
