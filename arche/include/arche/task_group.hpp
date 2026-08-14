#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace arche {

class TaskGroup final {
public:
  TaskGroup() = default;
  TaskGroup(const TaskGroup&) = delete;
  TaskGroup& operator=(const TaskGroup&) = delete;
  ~TaskGroup() { stop_and_join(); }

  template <typename Fn>
  void spawn(Fn&& function) {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      throw std::runtime_error("task group is stopping");
    }
    active_.fetch_add(1, std::memory_order_acq_rel);
    try {
      workers_.emplace_back(
          [this, fn = std::forward<Fn>(function)](std::stop_token stop) mutable {
          try {
            fn(stop);
          } catch (...) {
            std::lock_guard error_lock(error_mutex_);
            errors_.push_back(std::current_exception());
          }
          active_.fetch_sub(1, std::memory_order_acq_rel);
          finished_.notify_all();
          });
    } catch (...) {
      active_.fetch_sub(1, std::memory_order_acq_rel);
      throw;
    }
  }

  void request_stop() {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    for (auto& worker : workers_) {
      worker.request_stop();
    }
  }

  void stop_and_join() {
    std::vector<std::jthread> workers;
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      for (auto& worker : workers_) {
        worker.request_stop();
      }
      workers.swap(workers_);
    }
    workers.clear();
  }

  [[nodiscard]] bool stop_and_wait(std::chrono::milliseconds timeout) {
    request_stop();
    {
      std::unique_lock lock(mutex_);
      if (!finished_.wait_for(lock, timeout, [this] {
            return active_.load(std::memory_order_acquire) == 0;
          })) {
        return false;
      }
    }
    stop_and_join();
    return true;
  }

  void reset() {
    std::lock_guard lock(mutex_);
    if (!workers_.empty() || active_.load(std::memory_order_acquire) != 0) {
      throw std::runtime_error("cannot reset a running task group");
    }
    stopping_ = false;
  }

  [[nodiscard]] std::size_t active() const noexcept {
    return active_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::vector<std::exception_ptr> take_errors() {
    std::lock_guard lock(error_mutex_);
    auto result = std::move(errors_);
    errors_.clear();
    return result;
  }

private:
  mutable std::mutex mutex_;
  mutable std::mutex error_mutex_;
  std::condition_variable finished_;
  std::vector<std::jthread> workers_;
  std::vector<std::exception_ptr> errors_;
  std::atomic_size_t active_{0};
  bool stopping_{false};
};

} // namespace arche
