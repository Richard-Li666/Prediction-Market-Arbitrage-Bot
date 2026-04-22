#include "replay/timeline_merge.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>

namespace ll::replay {

namespace {

int64_t sort_key_from_row(const nlohmann::json& j) {
  if (j.contains("local_ts_mono_ns")) {
    return j.at("local_ts_mono_ns").get<int64_t>();
  }
  if (j.contains("exchange_ts_ms")) {
    return j.at("exchange_ts_ms").get<int64_t>() * 1'000'000LL;
  }
  return 0;
}

bool is_binance_trade(const LogEvent& e) {
  return e.source == "binance" && e.row.value("event_type", std::string{}) == "trade";
}

bool is_polymarket_quote(const LogEvent& e) {
  return e.source == "polymarket" && e.row.value("event_type", std::string{}) == "quote";
}

double poly_mid_from_row(const nlohmann::json& j) {
  const auto& p = j.at("payload");
  const double bid = p.at("best_bid").get<double>();
  const double ask = p.at("best_ask").get<double>();
  return 0.5 * (bid + ask);
}

}  // namespace

std::vector<LogEvent> load_jsonl(const std::string& path) {
  std::ifstream in(path);
  std::vector<LogEvent> out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    nlohmann::json j = nlohmann::json::parse(line);
    LogEvent ev;
    ev.sort_key_ns = sort_key_from_row(j);
    ev.source = j.value("source", std::string{});
    ev.row = std::move(j);
    out.push_back(std::move(ev));
  }
  std::sort(out.begin(), out.end(),
            [](const LogEvent& a, const LogEvent& b) { return a.sort_key_ns < b.sort_key_ns; });
  return out;
}

std::vector<LogEvent> merge_sorted(const std::vector<LogEvent>& a, const std::vector<LogEvent>& b) {
  std::vector<LogEvent> m;
  m.reserve(a.size() + b.size());
  m.insert(m.end(), a.begin(), a.end());
  m.insert(m.end(), b.begin(), b.end());
  std::sort(m.begin(), m.end(),
            [](const LogEvent& a, const LogEvent& b) { return a.sort_key_ns < b.sort_key_ns; });
  return m;
}

void print_alignment_stats(const std::vector<LogEvent>& merged, int64_t window_after_binance_ns) {
  std::size_t pairs = 0;
  int64_t sum_delay_ns = 0;
  int64_t min_delay_ns = std::numeric_limits<int64_t>::max();
  int64_t max_delay_ns = 0;

  for (std::size_t i = 0; i < merged.size(); ++i) {
    if (!is_binance_trade(merged[i])) {
      continue;
    }
    const int64_t t0 = merged[i].sort_key_ns;
    int64_t first_poly_ns = 0;
    bool found = false;
    for (std::size_t j = i + 1; j < merged.size(); ++j) {
      if (merged[j].sort_key_ns - t0 > window_after_binance_ns) {
        break;
      }
      if (is_polymarket_quote(merged[j])) {
        first_poly_ns = merged[j].sort_key_ns;
        found = true;
        break;
      }
    }
    if (!found) {
      continue;
    }
    ++pairs;
    const int64_t d = first_poly_ns - t0;
    sum_delay_ns += d;
    min_delay_ns = std::min(min_delay_ns, d);
    max_delay_ns = std::max(max_delay_ns, d);
  }

  std::cout << "alignment_window_ns=" << window_after_binance_ns << "\n";
  std::cout << "binance_trade_with_poly_quote_in_window=" << pairs << "\n";
  if (pairs == 0) {
    return;
  }
  std::cout << "avg_poly_lag_ns=" << (sum_delay_ns / static_cast<int64_t>(pairs)) << "\n";
  std::cout << "min_poly_lag_ns=" << min_delay_ns << "\n";
  std::cout << "max_poly_lag_ns=" << max_delay_ns << "\n";
}

}  // namespace ll::replay
