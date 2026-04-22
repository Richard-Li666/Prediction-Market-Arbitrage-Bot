#pragma once

#include <fstream>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace ll::logging {

class JsonlWriter {
 public:
  explicit JsonlWriter(std::string path);
  void append(const nlohmann::json& row);
  void flush();

 private:
  std::mutex mu_;
  std::ofstream out_;
};

}  // namespace ll::logging
