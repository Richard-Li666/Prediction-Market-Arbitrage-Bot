#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ll::telemetry {

class Pipeline {
 public:
  void mark(std::string key, int64_t mono_ns);
  std::string summary() const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, int64_t> last_ns_;
};

}  // namespace ll::telemetry
