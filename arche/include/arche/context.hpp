#pragma once

#include <arche/effect.hpp>
#include <arche/task_group.hpp>
#include <arche/types.hpp>

#include <axon/signal.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arche {

namespace detail {

struct CapabilityRecord {
  std::string id;
  std::string version;
  std::string interface_hash;
  std::type_index type{typeid(void)};
  std::shared_ptr<void> value;
  FiberId provider;
  std::atomic_bool available{false};
  bool multiple{false};
};

class CapabilityRegistry final {
public:
  std::shared_ptr<CapabilityRecord> insert(
      std::shared_ptr<CapabilityRecord> record);
  void erase(const std::shared_ptr<CapabilityRecord>& record);
  void set_available(const std::shared_ptr<CapabilityRecord>& record,
                     bool available);
  [[nodiscard]] std::shared_ptr<CapabilityRecord> resolve(
      std::string_view id, std::string_view range = "*",
      std::string_view interface_hash = {}) const;
  [[nodiscard]] std::vector<std::shared_ptr<CapabilityRecord>> by_provider(
      const FiberId& provider) const;
  [[nodiscard]] Json inspect() const;

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string,
                     std::vector<std::shared_ptr<CapabilityRecord>>>
      records_;
};

} // namespace detail

struct CapabilityDecision {
  bool allowed{true};
  std::string reason;
};

using CapabilityInterceptor =
    std::function<CapabilityDecision(std::string_view capability)>;

template <typename T>
class CapabilityLease final {
public:
  CapabilityLease() = default;

  [[nodiscard]] T* operator->() const noexcept {
    return static_cast<T*>(record_->value.get());
  }
  [[nodiscard]] T& operator*() const noexcept {
    return *static_cast<T*>(record_->value.get());
  }
  [[nodiscard]] explicit operator bool() const noexcept {
    return record_ && record_->available.load(std::memory_order_acquire);
  }
  [[nodiscard]] const FiberId& provider() const noexcept {
    return record_->provider;
  }
  [[nodiscard]] std::string_view version() const noexcept {
    return record_->version;
  }
  [[nodiscard]] std::shared_ptr<T> shared() const noexcept {
    if (!record_) return {};
    return std::shared_ptr<T>(record_, static_cast<T*>(record_->value.get()));
  }

private:
  friend class Context;
  explicit CapabilityLease(std::shared_ptr<detail::CapabilityRecord> record)
      : record_(std::move(record)) {}
  std::shared_ptr<detail::CapabilityRecord> record_;
};

class Context final : public std::enable_shared_from_this<Context> {
public:
  Context(ContextId id, std::string realm,
          std::shared_ptr<detail::CapabilityRegistry> registry,
          std::shared_ptr<Context> parent = {});

  [[nodiscard]] const ContextId& id() const noexcept { return id_; }
  [[nodiscard]] const std::string& realm() const noexcept { return realm_; }
  [[nodiscard]] std::shared_ptr<Context> parent() const noexcept {
    return parent_.lock();
  }

  [[nodiscard]] std::shared_ptr<Context> derive(std::string realm) const;
  void configure_owner(FiberId fiber, const PluginDescriptor& descriptor,
                       Json config = Json::object());
  [[nodiscard]] const Json& config() const noexcept { return config_; }
  void deny(std::string capability);
  void isolate(std::string capability, std::string realm);
  void intercept(std::string capability, CapabilityInterceptor interceptor);
  [[nodiscard]] bool permits(std::string_view capability) const;
  [[nodiscard]] Json explain_resolution(std::string_view capability,
                                        std::string_view range = "*") const;

  void bind_activation(FiberId fiber, const PluginDescriptor* descriptor,
                       EffectLedger* ledger,
                       std::vector<std::shared_ptr<detail::CapabilityRecord>>*
                           staged_provisions);
  void clear_activation() noexcept;

  template <typename T>
  void provide(std::string id, std::string version, std::shared_ptr<T> value,
               std::string interface_hash = {}) {
    ensure_activation();
    ensure_declared_provision(id);
    auto record = std::make_shared<detail::CapabilityRecord>();
    record->id = qualify(id);
    record->version = std::move(version);
    const auto declared = find_provision(id);
    record->interface_hash = interface_hash.empty() && declared
                                 ? declared->interface_hash
                                 : std::move(interface_hash);
    record->type = std::type_index(typeid(T));
    record->value = std::move(value);
    record->provider = active_fiber_;

    record->multiple = declared ? declared->multiple : false;
    registry_->insert(record);
    staged_provisions_->push_back(record);
    ledger_->add("provide:" + record->id,
                 [registry = registry_, record] { registry->erase(record); });
  }

  void provide_json(std::string id, std::string version, Json value);

  template <typename T>
  [[nodiscard]] CapabilityLease<T> require(std::string_view id,
                                           std::string_view range = "*") const {
    if (!permits(id)) {
      throw tokmon::Error("arche.capability.denied",
                          "capability denied by context: " + std::string(id));
    }
    ensure_declared_requirement(id);
    const auto declared = find_requirement(id);
    auto record = registry_->resolve(
        qualify_lookup(id), range,
        declared ? declared->interface_hash : std::string_view{});
    if (!record) {
      throw tokmon::Error("arche.capability.missing",
                          "capability is unavailable: " + std::string(id));
    }
    if (record->type != std::type_index(typeid(T))) {
      throw tokmon::Error("arche.capability.type",
                          "capability type mismatch: " + std::string(id));
    }
    return CapabilityLease<T>(std::move(record));
  }

  [[nodiscard]] std::optional<FiberId> resolve_provider(
      const CapabilityRequirement& requirement) const;

  void on_unload(std::string label, Undo undo);
  [[nodiscard]] TaskGroup& tasks() noexcept { return tasks_; }

  template <typename Event>
  [[nodiscard]] axon::Signal<Event>& signal(std::string key) {
    std::lock_guard lock(signal_mutex_);
    const auto qualified = qualify(std::move(key));
    auto iterator = signals_.find(qualified);
    if (iterator == signals_.end()) {
      auto value = std::make_shared<axon::Signal<Event>>();
      iterator = signals_.emplace(qualified, std::move(value)).first;
    }
    auto typed = std::static_pointer_cast<axon::Signal<Event>>(iterator->second);
    return *typed;
  }

  [[nodiscard]] Json inspect() const;

private:
  void ensure_activation() const;
  void ensure_declared_provision(std::string_view id) const;
  void ensure_declared_requirement(std::string_view id) const;
  [[nodiscard]] const CapabilityProvision* find_provision(
      std::string_view id) const;
  [[nodiscard]] const CapabilityRequirement* find_requirement(
      std::string_view id) const;
  [[nodiscard]] std::optional<std::string> isolated_realm(
      std::string_view id) const;
  [[nodiscard]] CapabilityDecision local_decision(
      std::string_view capability) const;
  [[nodiscard]] std::string qualify(std::string id) const;
  [[nodiscard]] std::string qualify_lookup(std::string_view id) const;

  ContextId id_;
  std::string realm_;
  std::shared_ptr<detail::CapabilityRegistry> registry_;
  std::weak_ptr<Context> parent_;
  mutable std::mutex policy_mutex_;
  std::set<std::string, std::less<>> denied_;
  std::map<std::string, std::string, std::less<>> isolation_;
  std::map<std::string, CapabilityInterceptor, std::less<>> interceptors_;
  std::optional<PluginDescriptor> owner_descriptor_;
  FiberId owner_fiber_;
  Json config_{Json::object()};

  FiberId active_fiber_;
  const PluginDescriptor* active_descriptor_{nullptr};
  EffectLedger* ledger_{nullptr};
  std::vector<std::shared_ptr<detail::CapabilityRecord>>* staged_provisions_{
      nullptr};
  TaskGroup tasks_;

  mutable std::mutex signal_mutex_;
  std::unordered_map<std::string, std::shared_ptr<void>> signals_;
};

} // namespace arche
