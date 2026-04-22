#include <iostream>
#include <string>

#include "replay/timeline_merge.hpp"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: replay_align <binance.jsonl> <polymarket.jsonl> [window_ms]\n";
    return 2;
  }

  const std::string bin_path = argv[1];
  const std::string poly_path = argv[2];
  int64_t window_ms = 1000;
  if (argc >= 4) {
    window_ms = std::stoll(argv[3]);
  }

  auto a = ll::replay::load_jsonl(bin_path);
  auto b = ll::replay::load_jsonl(poly_path);
  auto m = ll::replay::merge_sorted(a, b);

  const int64_t window_ns = window_ms * 1'000'000LL;
  ll::replay::print_alignment_stats(m, window_ns);
  return 0;
}
