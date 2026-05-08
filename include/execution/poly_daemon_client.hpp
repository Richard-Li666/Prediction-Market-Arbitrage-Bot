#pragma once

#include <string>

namespace ll::execution {

// Minimal bidirectional JSONL bridge to a long-running Polymarket trading daemon.
// The daemon is expected to:
// - print a single JSON line {"ok":true,"event":"ready",...} on startup
// - accept JSONL commands on stdin
// - respond with one JSON line per command on stdout
class PolyDaemonClient {
 public:
  PolyDaemonClient() = default;
  ~PolyDaemonClient();

  PolyDaemonClient(const PolyDaemonClient&) = delete;
  PolyDaemonClient& operator=(const PolyDaemonClient&) = delete;

  // Starts daemon (if not started). daemon_cmd is executed via /bin/sh -lc "<daemon_cmd>".
  // Returns false on failure, with an error message.
  bool start(const std::string& daemon_cmd, std::string* error_message);

  bool is_running() const { return running_; }

  // Sends a single JSON line and reads a single response JSON line.
  bool request_response_jsonl(const std::string& request_json_line, std::string* response_json_line,
                              std::string* error_message);

  void stop();

 private:
  bool running_{false};
  int child_pid_{-1};
  int to_child_fd_{-1};
  int from_child_fd_{-1};
};

}  // namespace ll::execution

