#pragma once

#include <arche/plugin.hpp>

#include <axon/signal.hpp>

#include <map>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace arche {

struct LifecycleEvent {
  FiberId fiber;
  std::string instance;
  FiberState from;
  FiberState to;
  CompositionEpoch epoch;
  std::string reason;
};

struct FiberSnapshot {
  FiberId id;
  std::string instance;
  PluginDescriptor descriptor;
  Json config{Json::object()};
  FiberState state{FiberState::inactive};
  bool mounted{false};
  std::string error;
  std::vector<FiberId> dependencies;
  std::vector<std::string> provisions;
  ContextId context_id;
  std::string realm;
  bool stuck{false};
  std::size_t active_tasks{0};
  std::size_t effect_count{0};
};

class Runtime final {
public:
  explicit Runtime(
      std::string realm = "root",
      std::chrono::milliseconds unload_timeout = std::chrono::seconds(5));
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  ~Runtime();

  [[nodiscard]] const RuntimeId& id() const noexcept { return id_; }
  [[nodiscard]] CompositionEpoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] std::shared_ptr<Context> root_context() const noexcept {
    return root_;
  }

  FiberId install(std::string instance, PluginPtr plugin,
                  Json config = Json::object(), std::string realm = {});
  void uninstall(std::string_view instance);
  void mount(std::string_view instance);
  void unmount(std::string_view instance);
  void reload(std::string_view instance, PluginPtr replacement,
              Json config = Json::object(), std::string realm = {});
  void settle();
  void shutdown();

  [[nodiscard]] std::optional<FiberSnapshot> fiber(
      std::string_view instance) const;
  [[nodiscard]] std::vector<FiberSnapshot> fibers() const;
  [[nodiscard]] std::shared_ptr<Context> fiber_context(
      std::string_view instance) const;
  [[nodiscard]] Json inspect() const;

  axon::Signal<LifecycleEvent>& lifecycle() noexcept { return lifecycle_; }

  template <typename T>
  void provide_root(std::string id, std::string version,
                    std::shared_ptr<T> value) {
    PluginDescriptor descriptor;
    descriptor.id = "arche.root";
    descriptor.version = "1.0.0";
    descriptor.provides.push_back({id, version, {}, true});
    std::vector<std::shared_ptr<detail::CapabilityRecord>> staged;
    EffectLedger* ledger = &root_ledger_;
    root_->bind_activation(FiberId("root"), &descriptor, ledger, &staged);
    root_->provide<T>(std::move(id), std::move(version), std::move(value));
    root_->clear_activation();
    for (const auto& record : staged) {
      registry_->set_available(record, true);
    }
  }

private:
  friend class Reconciler;
  struct Fiber;

  [[nodiscard]] Fiber& require_fiber(std::string_view instance);
  [[nodiscard]] const Fiber& require_fiber(std::string_view instance) const;
  [[nodiscard]] bool requirements_satisfied(
      Fiber& fiber, std::vector<FiberId>* providers = nullptr) const;
  void validate_dependency_graph();
  bool activate(Fiber& fiber);
  void unmount_recursive(Fiber& fiber, std::set<std::string>& visiting,
                         std::string reason);
  void transition(Fiber& fiber, FiberState to, std::string reason);
  [[nodiscard]] FiberSnapshot snapshot(const Fiber& fiber) const;
  void begin_composition_transaction();
  void commit_composition_transaction();
  void abort_composition_transaction() noexcept;
  void mark_composition_change();
  [[nodiscard]] PluginPtr plugin_for_reconcile(
      std::string_view instance) const;

  RuntimeId id_{tokmon::make_uuid()};
  std::shared_ptr<detail::CapabilityRegistry> registry_;
  std::shared_ptr<Context> root_;
  mutable std::recursive_mutex mutex_;
  std::map<std::string, std::unique_ptr<Fiber>, std::less<>> fibers_;
  EffectLedger root_ledger_;
  CompositionEpoch epoch_{0};
  bool composition_transaction_{false};
  bool composition_dirty_{false};
  bool stopping_{false};
  std::chrono::milliseconds unload_timeout_;
  axon::Signal<LifecycleEvent> lifecycle_;
};

} // namespace arche
