#include "core/thread_pool.hpp"

namespace ll::core {

ThreadPool::ThreadPool(std::size_t workers) {
  workers_.reserve(workers);
  for (std::size_t i = 0; i < workers; ++i) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) {
      t.join();
    }
  }
}

void ThreadPool::enqueue(std::function<void()> job) {
  inflight_.fetch_add(1, std::memory_order_acq_rel);
  {
    std::lock_guard<std::mutex> lk(mu_);
    tasks_.push(std::move(job));
  }
  cv_.notify_one();
}

void ThreadPool::wait_idle() {
  std::unique_lock<std::mutex> lk(idle_mu_);
  idle_cv_.wait(lk, [this] { return inflight_.load(std::memory_order_acquire) == 0; });
}

void ThreadPool::worker_loop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
      if (stop_ && tasks_.empty()) {
        return;
      }
      job = std::move(tasks_.front());
      tasks_.pop();
    }
    job();
    inflight_.fetch_sub(1, std::memory_order_acq_rel);
    idle_cv_.notify_all();
  }
}

}  // namespace ll::core
