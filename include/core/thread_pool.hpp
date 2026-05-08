#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef LEADLAG_POOL_USE_JTHREAD
#define LEADLAG_POOL_USE_JTHREAD 0
#endif

namespace ll::core {

/// Fixed-size worker pool aligned with course ThreadPool: tasks are `Task` values submitted via
/// `dispatch`. Workers prefer `std::jthread` when the toolchain supports it (CMake sets
/// `LEADLAG_POOL_USE_JTHREAD`); otherwise `std::thread` with explicit join (same queue semantics).
class ThreadPool {
 public:
  /// Callable wrapper for `dispatch` (course-style API).
  struct Task {
    std::function<void()> fn;

    Task() = default;

    template<typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, Task>>>
    explicit Task(F&& f) : fn(std::forward<F>(f)) {}

    void operator()() const {
      if (fn) {
        fn();
      }
    }
  };

  explicit ThreadPool(std::size_t workers);
  ~ThreadPool();

  void dispatch(Task task);
  void wait_idle();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

 private:
  void worker_loop();

#if LEADLAG_POOL_USE_JTHREAD
  std::vector<std::jthread> workers_;
#else
  std::vector<std::thread> workers_;
#endif
  std::queue<std::function<void()>> tasks_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_{false};
  std::mutex idle_mu_;
  std::condition_variable idle_cv_;
  std::atomic<int> inflight_{0};
};

}  // namespace ll::core
