#pragma once

#include <tokmon/common/types.hpp>

#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace arche {

using Undo = std::function<void()>;

struct EffectFailure {
  std::string label;
  std::string message;
};

class EffectLedger final {
public:
  EffectLedger() = default;
  EffectLedger(const EffectLedger&) = delete;
  EffectLedger& operator=(const EffectLedger&) = delete;

  ~EffectLedger() {
    if (!empty()) {
      (void)rollback();
    }
  }

  void add(std::string label, Undo undo) {
    if (!undo) {
      throw tokmon::Error("arche.effect.invalid",
                          "effect inverse must not be empty");
    }
    std::lock_guard lock(mutex_);
    if (rolling_back_) {
      throw tokmon::Error("arche.effect.closed",
                          "cannot add an effect during rollback");
    }
    effects_.push_back({std::move(label), std::move(undo)});
  }

  [[nodiscard]] std::vector<EffectFailure> rollback() noexcept {
    std::vector<Entry> pending;
    {
      std::lock_guard lock(mutex_);
      if (rolling_back_) {
        return {};
      }
      rolling_back_ = true;
      pending.swap(effects_);
    }

    std::vector<EffectFailure> failures;
    for (auto iterator = pending.rbegin(); iterator != pending.rend();
         ++iterator) {
      try {
        iterator->undo();
      } catch (const std::exception& error) {
        failures.push_back({iterator->label, error.what()});
      } catch (...) {
        failures.push_back({iterator->label, "unknown inverse failure"});
      }
    }
    {
      std::lock_guard lock(mutex_);
      rolling_back_ = false;
    }
    return failures;
  }

  [[nodiscard]] bool empty() const {
    std::lock_guard lock(mutex_);
    return effects_.empty();
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock(mutex_);
    return effects_.size();
  }

private:
  struct Entry {
    std::string label;
    Undo undo;
  };

  mutable std::mutex mutex_;
  std::vector<Entry> effects_;
  bool rolling_back_{false};
};

} // namespace arche

