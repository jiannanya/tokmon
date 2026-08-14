#include <arche/runtime.hpp>
#include <arche/manifest.hpp>

#include <algorithm>
#include <ranges>

namespace arche {

struct Runtime::Fiber {
  FiberId id{tokmon::make_uuid()};
  std::string instance;
  PluginPtr plugin;
  Json config{Json::object()};
  FiberState state{FiberState::inactive};
  std::shared_ptr<Context> context;
  std::unique_ptr<EffectLedger> effects;
  std::vector<FiberId> dependencies;
  std::vector<std::shared_ptr<detail::CapabilityRecord>> provisions;
  std::string error;
  bool mounted{false};
  bool stuck{false};
};

Runtime::Runtime(std::string realm,
                 std::chrono::milliseconds unload_timeout)
    : registry_(std::make_shared<detail::CapabilityRegistry>()),
      root_(std::make_shared<Context>(ContextId(tokmon::make_uuid()),
                                      std::move(realm), registry_)),
      unload_timeout_(unload_timeout) {}

Runtime::~Runtime() { shutdown(); }

FiberId Runtime::install(std::string instance, PluginPtr plugin, Json config,
                         std::string realm) {
  if (!plugin) {
    throw tokmon::Error("arche.install.plugin", "plugin must not be null");
  }
  validate_plugin_descriptor(plugin->descriptor());

  std::lock_guard lock(mutex_);
  if (fibers_.contains(instance)) {
    throw tokmon::Error("arche.install.duplicate",
                        "plugin instance already installed: " + instance);
  }
  auto fiber = std::make_unique<Fiber>();
  fiber->instance = std::move(instance);
  fiber->plugin = std::move(plugin);
  fiber->config = std::move(config);
  fiber->context = root_->derive(realm.empty() ? fiber->instance
                                               : std::move(realm));
  fiber->context->configure_owner(fiber->id, fiber->plugin->descriptor(),
                                  fiber->config);
  const auto id = fiber->id;
  fibers_.emplace(fiber->instance, std::move(fiber));
  mark_composition_change();
  return id;
}

void Runtime::uninstall(std::string_view instance) {
  std::lock_guard lock(mutex_);
  auto& target = require_fiber(instance);
  if (target.state != FiberState::inactive) {
    std::set<std::string> visiting;
    unmount_recursive(target, visiting, "uninstall");
  }
  if (target.stuck || target.state != FiberState::inactive) {
    throw tokmon::Error("arche.fiber.stuck",
                        "cannot uninstall a stuck fiber: " +
                            std::string(instance));
  }
  fibers_.erase(std::string(instance));
  mark_composition_change();
}

void Runtime::mount(std::string_view instance) {
  std::lock_guard lock(mutex_);
  auto& target = require_fiber(instance);
  const auto was_mounted = target.mounted;
  target.mounted = true;
  try {
    settle();
  } catch (...) {
    target.mounted = was_mounted;
    throw;
  }
  if (!was_mounted) mark_composition_change();
}

void Runtime::unmount(std::string_view instance) {
  std::lock_guard lock(mutex_);
  auto& target = require_fiber(instance);
  const auto was_mounted = target.mounted;
  target.mounted = false;
  std::set<std::string> visiting;
  unmount_recursive(target, visiting, "explicit unmount");
  settle();
  if (was_mounted) mark_composition_change();
}

void Runtime::reload(std::string_view instance, PluginPtr replacement,
                     Json config, std::string realm) {
  if (!replacement) {
    throw tokmon::Error("arche.reload.plugin", "replacement must not be null");
  }
  validate_plugin_descriptor(replacement->descriptor());
  std::lock_guard lock(mutex_);
  auto& target = require_fiber(instance);
  const auto was_mounted = target.mounted;
  std::set<std::string> visiting;
  unmount_recursive(target, visiting, "reload");
  if (target.stuck || target.state != FiberState::inactive) {
    throw tokmon::Error("arche.reload.stuck",
                        "cannot reload a fiber that failed to quiesce");
  }

  const auto previous = target.plugin;
  const auto previous_config = target.config;
  const auto previous_context = target.context;
  target.plugin = std::move(replacement);
  target.config = std::move(config);
  if (!realm.empty() && target.context->realm() != realm)
    target.context = root_->derive(std::move(realm));
  target.context->configure_owner(target.id, target.plugin->descriptor(),
                                  target.config);
  target.mounted = was_mounted;
  if (was_mounted && !activate(target)) {
    target.plugin = previous;
    target.config = previous_config;
    target.context = previous_context;
    target.context->configure_owner(target.id, target.plugin->descriptor(),
                                    target.config);
    target.error.clear();
    if (!activate(target)) {
      throw tokmon::Error(
          "arche.reload.rollback",
          "replacement and rollback activation both failed for: " +
              std::string(instance));
    }
    throw tokmon::Error("arche.reload.failed",
                        "replacement activation failed; old plugin restored");
  }
  settle();
  mark_composition_change();
}

PluginPtr Runtime::plugin_for_reconcile(std::string_view instance) const {
  std::lock_guard lock(mutex_);
  return require_fiber(instance).plugin;
}

void Runtime::settle() {
  std::lock_guard lock(mutex_);
  if (stopping_) {
    return;
  }
  validate_dependency_graph();
  bool changed = true;
  std::size_t iterations = 0;
  const auto limit = std::max<std::size_t>(16, fibers_.size() * 8);
  while (changed && iterations++ < limit) {
    changed = false;

    for (auto& [_, fiber] : fibers_) {
      if (fiber->state == FiberState::active && fiber->mounted &&
          !requirements_satisfied(*fiber)) {
        std::set<std::string> visiting;
        unmount_recursive(*fiber, visiting, "dependency withdrawn");
        changed = true;
      }
    }

    for (auto& [_, fiber] : fibers_) {
      if (fiber->state == FiberState::inactive && fiber->mounted &&
          requirements_satisfied(*fiber) && activate(*fiber)) {
        changed = true;
      }
    }
  }
  if (iterations >= limit && changed) {
    throw tokmon::Error("arche.settle.divergent",
                        "composition failed to reach quiescence");
  }
}

void Runtime::shutdown() {
  std::lock_guard lock(mutex_);
  if (stopping_) {
    return;
  }
  stopping_ = true;
  std::set<std::string> visiting;
  for (auto iterator = fibers_.rbegin(); iterator != fibers_.rend();
       ++iterator) {
    iterator->second->mounted = false;
    unmount_recursive(*iterator->second, visiting, "runtime shutdown");
  }
  for (auto iterator = fibers_.begin(); iterator != fibers_.end();) {
    if (iterator->second->stuck) {
      // A stuck native task may still reference the plugin/context. Logical
      // unload therefore quarantines ownership until process exit rather than
      // manufacturing a use-after-free.
      (void)iterator->second.release();
      iterator = fibers_.erase(iterator);
    } else {
      iterator = fibers_.erase(iterator);
    }
  }
  (void)root_ledger_.rollback();
}

std::optional<FiberSnapshot> Runtime::fiber(std::string_view instance) const {
  std::lock_guard lock(mutex_);
  const auto iterator = fibers_.find(instance);
  if (iterator == fibers_.end()) {
    return std::nullopt;
  }
  return snapshot(*iterator->second);
}

std::vector<FiberSnapshot> Runtime::fibers() const {
  std::lock_guard lock(mutex_);
  std::vector<FiberSnapshot> result;
  result.reserve(fibers_.size());
  for (const auto& [_, fiber] : fibers_) {
    result.push_back(snapshot(*fiber));
  }
  return result;
}

std::shared_ptr<Context> Runtime::fiber_context(
    std::string_view instance) const {
  std::lock_guard lock(mutex_);
  return require_fiber(instance).context;
}

Json Runtime::inspect() const {
  Json result{{"runtime_id", id_.str()},
              {"composition_epoch", epoch_},
              {"context", root_->inspect()},
              {"fibers", Json::array()}};
  for (const auto& value : fibers()) {
    result["fibers"].push_back(
        {{"id", value.id.str()},
         {"instance", value.instance},
         {"plugin_id", value.descriptor.id},
         {"plugin_version", value.descriptor.version},
         {"state", to_string(value.state)},
         {"stuck", value.stuck},
         {"error", value.error},
         {"context_id", value.context_id.str()},
         {"realm", value.realm},
         {"active_tasks", value.active_tasks},
         {"effect_count", value.effect_count},
         {"dependencies", value.dependencies},
         {"provisions", value.provisions}});
  }
  return result;
}

Runtime::Fiber& Runtime::require_fiber(std::string_view instance) {
  const auto iterator = fibers_.find(instance);
  if (iterator == fibers_.end()) {
    throw tokmon::Error("arche.fiber.missing",
                        "unknown plugin instance: " + std::string(instance));
  }
  return *iterator->second;
}

const Runtime::Fiber& Runtime::require_fiber(std::string_view instance) const {
  const auto iterator = fibers_.find(instance);
  if (iterator == fibers_.end()) {
    throw tokmon::Error("arche.fiber.missing",
                        "unknown plugin instance: " + std::string(instance));
  }
  return *iterator->second;
}

bool Runtime::requirements_satisfied(Fiber& fiber,
                                     std::vector<FiberId>* providers) const {
  std::vector<FiberId> resolved;
  for (const auto& requirement : fiber.plugin->descriptor().requirements) {
    const auto provider = fiber.context->resolve_provider(requirement);
    if (!provider) {
      if (!requirement.optional) {
        fiber.error = "unsatisfied requirement: " + requirement.capability +
                      " " + requirement.range + "; " +
                      fiber.context
                          ->explain_resolution(requirement.capability,
                                               requirement.range)
                          .dump();
        return false;
      }
      continue;
    }
    resolved.push_back(*provider);
  }
  if (providers) {
    *providers = std::move(resolved);
  }
  fiber.error.clear();
  return true;
}

void Runtime::validate_dependency_graph() {
  std::map<std::string, std::vector<Fiber*>, std::less<>> providers;
  for (auto& [_, fiber] : fibers_) {
    if (!fiber->mounted) continue;
    for (const auto& provision : fiber->plugin->descriptor().provides)
      providers[provision.capability].push_back(fiber.get());
  }
  std::map<std::string, std::vector<std::string>, std::less<>> edges;
  for (auto& [instance, fiber] : fibers_) {
    if (!fiber->mounted) continue;
    for (const auto& requirement : fiber->plugin->descriptor().requirements) {
      const auto found = providers.find(requirement.capability);
      if (found == providers.end()) continue;
      for (const auto* provider : found->second) {
        const auto provision = std::ranges::find(
            provider->plugin->descriptor().provides,
            requirement.capability, &CapabilityProvision::capability);
        if (provision != provider->plugin->descriptor().provides.end() &&
            version_satisfies(provision->version, requirement.range) &&
            (requirement.interface_hash.empty() ||
             requirement.interface_hash == provision->interface_hash)) {
          edges[instance].push_back(provider->instance);
        }
      }
    }
  }
  enum class Mark { unseen, visiting, complete };
  std::map<std::string, Mark, std::less<>> marks;
  std::vector<std::string> stack;
  std::function<void(const std::string&)> visit = [&](const std::string& node) {
    if (marks[node] == Mark::complete) return;
    if (marks[node] == Mark::visiting) {
      const auto start = std::ranges::find(stack, node);
      std::string cycle;
      for (auto iterator = start; iterator != stack.end(); ++iterator)
        cycle += (cycle.empty() ? "" : " -> ") + *iterator;
      cycle += " -> " + node;
      for (auto iterator = start; iterator != stack.end(); ++iterator)
        fibers_.at(*iterator)->error = "dependency cycle: " + cycle;
      throw tokmon::Error("arche.dependency.cycle",
                          "mounted composition contains a cycle: " + cycle);
    }
    marks[node] = Mark::visiting;
    stack.push_back(node);
    for (const auto& dependency : edges[node]) visit(dependency);
    stack.pop_back();
    marks[node] = Mark::complete;
  };
  for (const auto& [instance, _] : edges) visit(instance);
}

bool Runtime::activate(Fiber& fiber) {
  if (fiber.state != FiberState::inactive || !fiber.mounted) {
    return false;
  }
  std::vector<FiberId> providers;
  if (!requirements_satisfied(fiber, &providers)) {
    return false;
  }

  transition(fiber, FiberState::reloading, "requirements satisfied");
  fiber.context->tasks().reset();
  fiber.effects = std::make_unique<EffectLedger>();
  fiber.provisions.clear();
  fiber.dependencies = providers;
  fiber.error.clear();
  fiber.stuck = false;

  try {
    fiber.context->bind_activation(fiber.id, &fiber.plugin->descriptor(),
                                   fiber.effects.get(), &fiber.provisions);
    fiber.plugin->apply(*fiber.context);
    fiber.context->clear_activation();
    for (const auto& provision : fiber.provisions) {
      registry_->set_available(provision, true);
    }
    transition(fiber, FiberState::active, "apply committed");
    return true;
  } catch (const std::exception& error) {
    fiber.context->clear_activation();
    for (const auto& provision : fiber.provisions) {
      registry_->set_available(provision, false);
    }
    const auto failures = fiber.effects->rollback();
    fiber.provisions.clear();
    fiber.dependencies.clear();
    fiber.error = error.what();
    if (!failures.empty()) {
      fiber.error += "; inverse failures: " +
                     std::to_string(failures.size());
    }
    transition(fiber, FiberState::inactive, "apply failed");
    return false;
  }
}

void Runtime::unmount_recursive(Fiber& fiber,
                                std::set<std::string>& visiting,
                                std::string reason) {
  if (fiber.state == FiberState::inactive) {
    return;
  }
  if (!visiting.insert(fiber.instance).second) {
    throw tokmon::Error("arche.dependency.cycle",
                        "cycle detected while unloading: " + fiber.instance);
  }

  for (const auto& provision : fiber.provisions) {
    registry_->set_available(provision, false);
  }

  for (auto& [_, candidate] : fibers_) {
    if (candidate.get() == &fiber ||
        candidate->state == FiberState::inactive) {
      continue;
    }
    if (std::ranges::find(candidate->dependencies, fiber.id) !=
        candidate->dependencies.end()) {
      unmount_recursive(*candidate, visiting,
                        "provider unavailable: " + fiber.instance);
      if (candidate->state != FiberState::inactive) {
        fiber.stuck = true;
        fiber.error = "dependent failed to quiesce: " + candidate->instance;
        lifecycle_.emit({fiber.id, fiber.instance, fiber.state, fiber.state,
                         epoch_, "stuck: " + fiber.error});
        visiting.erase(fiber.instance);
        return;
      }
    }
  }

  transition(fiber, FiberState::unloading, std::move(reason));
  try {
    fiber.context->tasks().request_stop();
    fiber.plugin->quiesce(*fiber.context);
    if (!fiber.context->tasks().stop_and_wait(unload_timeout_)) {
      fiber.stuck = true;
      fiber.error = "task group did not quiesce before unload deadline";
      lifecycle_.emit({fiber.id, fiber.instance, fiber.state, fiber.state,
                       epoch_, "stuck: " + fiber.error});
      visiting.erase(fiber.instance);
      return;
    }
  } catch (const std::exception& error) {
    fiber.error = error.what();
  }

  if (fiber.effects) {
    const auto failures = fiber.effects->rollback();
    if (!failures.empty()) {
      fiber.error = "inverse failures: " + std::to_string(failures.size());
    }
  }
  fiber.effects.reset();
  fiber.provisions.clear();
  fiber.dependencies.clear();
  fiber.stuck = false;
  transition(fiber, FiberState::inactive, "effects reverted");
  visiting.erase(fiber.instance);
}

void Runtime::transition(Fiber& fiber, FiberState to, std::string reason) {
  const auto from = fiber.state;
  fiber.state = to;
  lifecycle_.emit({fiber.id, fiber.instance, from, to, epoch_,
                   std::move(reason)});
}

void Runtime::begin_composition_transaction() {
  if (composition_transaction_) {
    throw tokmon::Error("arche.composition.nested",
                        "composition transactions cannot be nested");
  }
  composition_transaction_ = true;
  composition_dirty_ = false;
}

void Runtime::commit_composition_transaction() {
  if (!composition_transaction_) {
    throw tokmon::Error("arche.composition.transaction",
                        "no composition transaction is active");
  }
  if (composition_dirty_) ++epoch_;
  composition_transaction_ = false;
  composition_dirty_ = false;
}

void Runtime::abort_composition_transaction() noexcept {
  composition_transaction_ = false;
  composition_dirty_ = false;
}

void Runtime::mark_composition_change() {
  if (composition_transaction_) composition_dirty_ = true;
  else ++epoch_;
}

FiberSnapshot Runtime::snapshot(const Fiber& fiber) const {
  FiberSnapshot result;
  result.id = fiber.id;
  result.instance = fiber.instance;
  result.descriptor = fiber.plugin->descriptor();
  result.config = fiber.config;
  result.state = fiber.state;
  result.mounted = fiber.mounted;
  result.error = fiber.error;
  result.dependencies = fiber.dependencies;
  result.context_id = fiber.context->id();
  result.realm = fiber.context->realm();
  result.stuck = fiber.stuck;
  result.active_tasks = fiber.context->tasks().active();
  result.effect_count = fiber.effects ? fiber.effects->size() : 0;
  for (const auto& provision : fiber.provisions) {
    result.provisions.push_back(provision->id);
  }
  return result;
}

} // namespace arche
