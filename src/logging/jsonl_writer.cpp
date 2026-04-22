#include "logging/jsonl_writer.hpp"

#include <nlohmann/json.hpp>

namespace ll::logging {

JsonlWriter::JsonlWriter(std::string path) : out_(path, std::ios::out | std::ios::app) {}

void JsonlWriter::append(const nlohmann::json& row) {
  std::lock_guard<std::mutex> lk(mu_);
  out_ << row.dump() << '\n';
  out_.flush();
}

void JsonlWriter::flush() {
  std::lock_guard<std::mutex> lk(mu_);
  out_.flush();
}

}  // namespace ll::logging
