#pragma once

#include <axon/executor.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <tuple>
#include <utility>
#include <vector>

namespace axon {

class Lifetime final {
public:
  Lifetime() : token_(std::make_shared<int>(0)) {}
  Lifetime(const Lifetime&) = delete;
  Lifetime& operator=(const Lifetime&) = delete;
  Lifetime(Lifetime&&) noexcept = default;
  Lifetime& operator=(Lifetime&&) noexcept = default;

  [[nodiscard]] std::weak_ptr<void> weak() const noexcept { return token_; }
  void expire() noexcept { token_.reset(); }

private:
  std::shared_ptr<void> token_;
};

class Connection final {
public:
  Connection() = default;
  explicit Connection(std::function<void()> disconnect)
      : disconnect_(std::move(disconnect)) {}
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  Connection(Connection&& other) noexcept
      : disconnect_(std::exchange(other.disconnect_, {})) {}
  Connection& operator=(Connection&& other) noexcept {
    if (this != &other) {
      disconnect();
      disconnect_ = std::exchange(other.disconnect_, {});
    }
    return *this;
  }

  ~Connection() { disconnect(); }

  void disconnect() noexcept {
    if (auto callback = std::exchange(disconnect_, {})) {
      callback();
    }
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(disconnect_);
  }

private:
  std::function<void()> disconnect_;
};

template <typename... Args>
class Signal final {
  struct Slot {
    std::uint64_t id{};
    std::function<void(Args...)> callback;
    std::weak_ptr<void> owner;
    bool owner_bound{false};
    bool once{false};
    std::atomic_bool connected{true};
  };

  struct State {
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Slot>> slots;
    std::function<void(std::exception_ptr)> error_sink;
    std::uint64_t next_id{1};
  };

public:
  Signal() : state_(std::make_shared<State>()) {}
  Signal(const Signal&) = delete;
  Signal& operator=(const Signal&) = delete;
  Signal(Signal&&) noexcept = default;
  Signal& operator=(Signal&&) noexcept = default;

  template <typename Fn>
  [[nodiscard]] Connection connect(Fn&& callback) {
    return add(std::forward<Fn>(callback), {}, false, false);
  }

  template <typename Fn>
  [[nodiscard]] Connection connect_once(Fn&& callback) {
    return add(std::forward<Fn>(callback), {}, false, true);
  }

  template <typename Fn>
  [[nodiscard]] Connection connect(const Lifetime& owner, Fn&& callback) {
    return add(std::forward<Fn>(callback), owner.weak(), true, false);
  }

  void set_error_sink(std::function<void(std::exception_ptr)> sink) {
    std::lock_guard lock(state_->mutex);
    state_->error_sink = std::move(sink);
  }

  void emit(Args... args) const {
    std::vector<std::shared_ptr<Slot>> snapshot;
    std::function<void(std::exception_ptr)> error_sink;
    {
      std::lock_guard lock(state_->mutex);
      snapshot = state_->slots;
      error_sink = state_->error_sink;
    }

    bool cleanup = false;
    for (const auto& slot : snapshot) {
      if (!slot->connected.load(std::memory_order_acquire)) {
        cleanup = true;
        continue;
      }
      if (slot->owner_bound && slot->owner.expired()) {
        slot->connected.store(false, std::memory_order_release);
        cleanup = true;
        continue;
      }
      if (slot->once &&
          slot->connected.exchange(false, std::memory_order_acq_rel) == false) {
        cleanup = true;
        continue;
      }
      try {
        slot->callback(args...);
      } catch (...) {
        if (error_sink) {
          error_sink(std::current_exception());
        }
      }
      cleanup = cleanup || slot->once;
    }

    if (cleanup) {
      compact();
    }
  }

  bool emit_queued(Executor& executor, Args... args) const {
    return emit_queued(executor, std::stop_token{}, std::forward<Args>(args)...);
  }

  bool emit_queued(Executor& executor, std::stop_token stop,
                   Args... args) const {
    std::vector<std::shared_ptr<Slot>> snapshot;
    std::function<void(std::exception_ptr)> error_sink;
    {
      std::lock_guard lock(state_->mutex);
      snapshot = state_->slots;
      error_sink = state_->error_sink;
    }
    auto values = std::make_tuple(std::forward<Args>(args)...);
    return executor.post([snapshot = std::move(snapshot),
                          error_sink = std::move(error_sink), stop,
                          values = std::move(values)]() mutable {
      if (stop.stop_requested()) return;
      for (const auto& slot : snapshot) {
        if (!slot->connected.load(std::memory_order_acquire) ||
            (slot->owner_bound && slot->owner.expired())) {
          continue;
        }
        if (slot->once &&
            !slot->connected.exchange(false, std::memory_order_acq_rel)) {
          continue;
        }
        try {
          std::apply(slot->callback, values);
        } catch (...) {
          if (error_sink) error_sink(std::current_exception());
        }
      }
    });
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock(state_->mutex);
    return static_cast<std::size_t>(std::count_if(
        state_->slots.begin(), state_->slots.end(), [](const auto& slot) {
          return slot->connected.load(std::memory_order_acquire) &&
                 (!slot->owner_bound || !slot->owner.expired());
        }));
  }

  void clear() {
    std::lock_guard lock(state_->mutex);
    for (const auto& slot : state_->slots) {
      slot->connected.store(false, std::memory_order_release);
    }
    state_->slots.clear();
  }

private:
  explicit Signal(std::shared_ptr<State> state) : state_(std::move(state)) {}

  template <typename Fn>
  Connection add(Fn&& callback, std::weak_ptr<void> owner, bool owner_bound,
                 bool once) {
    auto slot = std::make_shared<Slot>();
    slot->callback = std::forward<Fn>(callback);
    slot->owner = std::move(owner);
    slot->owner_bound = owner_bound;
    slot->once = once;
    {
      std::lock_guard lock(state_->mutex);
      slot->id = state_->next_id++;
      state_->slots.push_back(slot);
    }

    std::weak_ptr<State> weak_state = state_;
    std::weak_ptr<Slot> weak_slot = slot;
    return Connection([weak_state, weak_slot] {
      if (auto value = weak_slot.lock()) {
        value->connected.store(false, std::memory_order_release);
      }
      if (auto state = weak_state.lock()) {
        std::lock_guard lock(state->mutex);
        std::erase_if(state->slots, [](const auto& candidate) {
          return !candidate->connected.load(std::memory_order_acquire);
        });
      }
    });
  }

  void compact() const {
    std::lock_guard lock(state_->mutex);
    std::erase_if(state_->slots, [](const auto& slot) {
      return !slot->connected.load(std::memory_order_acquire) ||
             (slot->owner_bound && slot->owner.expired());
    });
  }

  std::shared_ptr<State> state_;
};

} // namespace axon
