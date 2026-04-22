#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "core/clock.hpp"
#include "execution/executor.hpp"
#include "replay/timeline_merge.hpp"
#include "signals/engine.hpp"
#include "sim/latency_backtest.hpp"

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: paper_trader <config.json> <binance.jsonl> <polymarket.jsonl>\n";
    return 2;
  }

  const std::string cfg_path = argv[1];
  const std::string bin_path = argv[2];
  const std::string poly_path = argv[3];

  std::ifstream in(cfg_path);
  if (!in) {
    std::cerr << "cannot open config: " << cfg_path << "\n";
    return 1;
  }
  const auto j = nlohmann::json::parse(in);

  ll::signals::SignalConfig s_cfg;
  s_cfg.move_bps = j.at("signal").at("move_bps").get<double>();

  ll::sim::SimConfig sim_cfg;
  sim_cfg.latencies_ms = j.at("sim").at("latencies_ms").get<std::vector<int>>();
  sim_cfg.fee_bps = j.at("sim").at("fee_bps").get<double>();
  sim_cfg.slippage_bps = j.at("sim").at("slippage_bps").get<double>();
  sim_cfg.missed_fill_probability = j.at("sim").at("missed_fill_probability").get<double>();
  if (j.at("sim").contains("exit_horizon_ms")) {
    sim_cfg.exit_horizon_ms = j.at("sim").at("exit_horizon_ms").get<int>();
  }

  const std::string token = j.value("polymarket_token_id", std::string{"unknown_token"});

  auto bin = ll::replay::load_jsonl(bin_path);
  auto poly = ll::replay::load_jsonl(poly_path);
  auto merged = ll::replay::merge_sorted(bin, poly);
  const auto sigs = ll::signals::detect_binance_moves(bin, s_cfg);
  const auto rows = ll::sim::run_latency_sweep(sigs, merged, sim_cfg);

  ll::execution::PaperExecutor paper;
  for (const auto& r : rows) {
    ll::execution::OrderIntent o;
    o.mono_ns = ll::core::steady_ns();
    o.side = "PAPER_SWEEP";
    o.limit_price = 0.0;
    o.qty = static_cast<double>(r.fills);
    o.market_token_id = token;
    paper.record_intent(o);
    std::cout << "latency_ms=" << r.latency_ms << " fills=" << r.fills
              << " pnl_prob_pts=" << r.pnl_probability_points << "\n";
  }

  return 0;
}
