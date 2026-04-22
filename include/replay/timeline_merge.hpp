#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ll::replay {

struct LogEvent {
  int64_t sort_key_ns = 0;
  std::string source;
  nlohmann::json row;
};

std::vector<LogEvent> load_jsonl(const std::string& path);

std::vector<LogEvent> merge_sorted(const std::vector<LogEvent>& a, const std::vector<LogEvent>& b);

void print_alignment_stats(const std::vector<LogEvent>& merged, int64_t window_after_binance_ns);

}  // namespace ll::replay
