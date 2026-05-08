#include "execution/poly_daemon_client.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ll::execution {
namespace {

static bool write_all(int fd, const char* buf, std::size_t n, std::string* err) {
  std::size_t off = 0;
  while (off < n) {
    const ssize_t w = ::write(fd, buf + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (err) *err = std::string("write failed: ") + std::strerror(errno);
      return false;
    }
    off += static_cast<std::size_t>(w);
  }
  return true;
}

static bool read_line(int fd, std::string* out, std::string* err) {
  out->clear();
  char c = 0;
  for (;;) {
    const ssize_t r = ::read(fd, &c, 1);
    if (r == 0) {
      if (err) *err = "read EOF from daemon";
      return false;
    }
    if (r < 0) {
      if (errno == EINTR) continue;
      if (err) *err = std::string("read failed: ") + std::strerror(errno);
      return false;
    }
    if (c == '\n') {
      return true;
    }
    out->push_back(c);
    if (out->size() > 2 * 1024 * 1024) {
      if (err) *err = "daemon response too large (>2MB)";
      return false;
    }
  }
}

static void close_fd(int* fd) {
  if (*fd >= 0) {
    ::close(*fd);
    *fd = -1;
  }
}

}  // namespace

PolyDaemonClient::~PolyDaemonClient() { stop(); }

bool PolyDaemonClient::start(const std::string& daemon_cmd, std::string* error_message) {
  if (running_) return true;
  if (daemon_cmd.empty()) {
    if (error_message) *error_message = "POLY_DAEMON_CMD is empty";
    return false;
  }

  int to_child[2]{-1, -1};
  int from_child[2]{-1, -1};
  if (::pipe(to_child) != 0) {
    if (error_message) *error_message = std::string("pipe(to_child) failed: ") + std::strerror(errno);
    return false;
  }
  if (::pipe(from_child) != 0) {
    if (error_message) *error_message = std::string("pipe(from_child) failed: ") + std::strerror(errno);
    ::close(to_child[0]);
    ::close(to_child[1]);
    return false;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    if (error_message) *error_message = std::string("fork failed: ") + std::strerror(errno);
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);
    return false;
  }

  if (pid == 0) {
    // Child: stdin <- to_child[0], stdout -> from_child[1]
    ::dup2(to_child[0], STDIN_FILENO);
    ::dup2(from_child[1], STDOUT_FILENO);
    // Keep stderr as-is so errors are visible in terminal.
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);

    execl("/bin/sh", "sh", "-lc", daemon_cmd.c_str(), (char*)nullptr);
    _exit(127);
  }

  // Parent
  child_pid_ = static_cast<int>(pid);
  to_child_fd_ = to_child[1];
  from_child_fd_ = from_child[0];
  ::close(to_child[0]);
  ::close(from_child[1]);

  // Wait for daemon ready line.
  std::string ready;
  std::string err;
  if (!read_line(from_child_fd_, &ready, &err)) {
    if (error_message) *error_message = "daemon did not become ready: " + err;
    stop();
    return false;
  }
  if (ready.find("\"ok\"") == std::string::npos || ready.find("ready") == std::string::npos) {
    if (error_message) *error_message = "daemon unexpected first line: " + ready;
    stop();
    return false;
  }

  running_ = true;
  return true;
}

bool PolyDaemonClient::request_response_jsonl(const std::string& request_json_line,
                                              std::string* response_json_line, std::string* error_message) {
  if (!running_) {
    if (error_message) *error_message = "daemon not running";
    return false;
  }
  std::string err;
  const std::string line = request_json_line + "\n";
  if (!write_all(to_child_fd_, line.data(), line.size(), &err)) {
    if (error_message) *error_message = err;
    return false;
  }
  if (!read_line(from_child_fd_, response_json_line, &err)) {
    if (error_message) *error_message = err;
    return false;
  }
  return true;
}

void PolyDaemonClient::stop() {
  if (!running_) {
    close_fd(&to_child_fd_);
    close_fd(&from_child_fd_);
    child_pid_ = -1;
    return;
  }
  running_ = false;
  close_fd(&to_child_fd_);
  close_fd(&from_child_fd_);

  if (child_pid_ > 0) {
    // Try graceful termination, then reap.
    ::kill(child_pid_, SIGTERM);
    int status = 0;
    (void)::waitpid(child_pid_, &status, 0);
  }
  child_pid_ = -1;
}

}  // namespace ll::execution

