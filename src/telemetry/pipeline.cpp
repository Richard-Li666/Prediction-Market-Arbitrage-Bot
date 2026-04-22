#include "telemetry/pipeline.hpp"

#include <sstream>

namespace ll::telemetry {

void Pipeline::mark(std::string key, int64_t mono_ns) {
  std::lock_guard<std::mutex> lk(mu_);
  last_ns_[std::move(key)] = mono_ns;
}

std::string Pipeline::summary() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::ostringstream oss;
  for (const auto& [k, v] : last_ns_) {
    oss << k << "=" << v << ";";
  }
  return oss.str();
}

}  // namespace ll::telemetry
