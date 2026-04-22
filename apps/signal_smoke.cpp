#include <algorithm>
#include <iostream>
#include <string>

#include "replay/timeline_merge.hpp"
#include "signals/engine.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: signal_smoke <binance.jsonl> [move_bps]\n";
    return 2;
  }

  const std::string path = argv[1];
  ll::signals::SignalConfig cfg;
  if (argc >= 3) {
    cfg.move_bps = std::stod(argv[2]);
  }

  const auto ev = ll::replay::load_jsonl(path);
  const auto sigs = ll::signals::detect_binance_moves(ev, cfg);
  std::cout << "signals=" << sigs.size() << " move_bps>=" << cfg.move_bps << "\n";
  for (std::size_t i = 0; i < std::min<std::size_t>(10, sigs.size()); ++i) {
    std::cout << "sig[" << i << "] mono_ns=" << sigs[i].at_mono_ns << " px=" << sigs[i].price
              << " sym=" << sigs[i].symbol << "\n";
  }
  return 0;
}
