#include <atomic>

#include "core/thread_pool.hpp"

int main() {
  constexpr int kWorkers = 4;
  constexpr int kTasks = 1000;

  ll::core::ThreadPool pool(static_cast<std::size_t>(kWorkers));
  std::atomic<int> sum{0};

  for (int i = 0; i < kTasks; ++i) {
    pool.dispatch(ll::core::ThreadPool::Task{[&sum, i] {
      sum.fetch_add(i, std::memory_order_relaxed);
    }});
  }

  pool.wait_idle();

  const int expected = (kTasks - 1) * kTasks / 2;
  if (sum.load() != expected) {
    return 1;
  }
  return 0;
}
