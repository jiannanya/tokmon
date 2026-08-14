#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace axon {

using Task = std::function<void()>;

class Executor {
public:
  virtual ~Executor() = default;
  virtual bool post(Task task) = 0;
};

class InlineExecutor final : public Executor {
public:
  bool post(Task task) override {
    task();
    return true;
  }
};

class Strand final : public Executor {
public:
  explicit Strand(Executor& underlying) : underlying_(underlying) {}
  Strand(const Strand&) = delete;
  Strand& operator=(const Strand&) = delete;

  bool post(Task task) override {
    bool schedule = false;
    {
      std::lock_guard lock(mutex_);
      if (stopping_) return false;
      tasks_.push_back(std::move(task));
      if (!running_) {
        running_ = true;
        schedule = true;
      }
    }
    if (!schedule) return true;
    if (underlying_.post([this] { drain(); })) return true;
    std::lock_guard lock(mutex_);
    running_ = false;
    tasks_.clear();
    return false;
  }

  void shutdown() {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    tasks_.clear();
  }

  [[nodiscard]] std::size_t pending() const {
    std::lock_guard lock(mutex_);
    return tasks_.size();
  }

private:
  void drain() {
    for (;;) {
      Task task;
      {
        std::lock_guard lock(mutex_);
        if (stopping_ || tasks_.empty()) {
          running_ = false;
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      try {
        task();
      } catch (...) {
        // The wrapped executor owns its error reporting policy.
      }
    }
  }

  Executor& underlying_;
  mutable std::mutex mutex_;
  std::deque<Task> tasks_;
  bool running_{false};
  bool stopping_{false};
};

class ThreadPool final : public Executor {
public:
  explicit ThreadPool(std::size_t threads =
                          std::max<std::size_t>(1, std::thread::hardware_concurrency())) {
    workers_.reserve(threads);
    for (std::size_t index = 0; index < threads; ++index) {
      workers_.emplace_back([this](std::stop_token stop) { run(stop); });
    }
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  ~ThreadPool() override { shutdown(); }

  bool post(Task task) override {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        return false;
      }
      tasks_.push_back(std::move(task));
    }
    ready_.notify_one();
    return true;
  }

  void shutdown() {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        return;
      }
      stopping_ = true;
    }
    for (auto& worker : workers_) {
      worker.request_stop();
    }
    ready_.notify_all();
    workers_.clear();
    std::lock_guard lock(mutex_);
    tasks_.clear();
  }

  [[nodiscard]] std::size_t pending() const {
    std::lock_guard lock(mutex_);
    return tasks_.size();
  }

private:
  void run(std::stop_token stop) {
    while (!stop.stop_requested()) {
      Task task;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, stop,
                    [this] { return stopping_ || !tasks_.empty(); });
        if ((stopping_ || stop.stop_requested()) && tasks_.empty()) {
          return;
        }
        if (tasks_.empty()) {
          continue;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      try {
        task();
      } catch (...) {
        if (error_sink_) {
          error_sink_(std::current_exception());
        }
      }
    }
  }

public:
  void set_error_sink(std::function<void(std::exception_ptr)> sink) {
    std::lock_guard lock(mutex_);
    error_sink_ = std::move(sink);
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable_any ready_;
  std::deque<Task> tasks_;
  std::vector<std::jthread> workers_;
  std::function<void(std::exception_ptr)> error_sink_;
  bool stopping_{false};
};

} // namespace axon
