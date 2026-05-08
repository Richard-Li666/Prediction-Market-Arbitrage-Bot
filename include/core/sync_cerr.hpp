#pragma once

#include <mutex>

namespace ll::io {

/// `std::cerr` is not thread-safe; use this for any cross-thread logging to stderr.
inline std::recursive_mutex& cerr_mutex() {
  static std::recursive_mutex m;
  return m;
}

struct SyncCerrLock {
  std::lock_guard<std::recursive_mutex> lock_;
  SyncCerrLock() : lock_(cerr_mutex()) {}
};

}  // namespace ll::io
