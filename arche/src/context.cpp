#include <arche/context.hpp>

#include <algorithm>

namespace arche::detail {

std::shared_ptr<CapabilityRecord> CapabilityRegistry::insert(
    std::shared_ptr<CapabilityRecord> record) {
  std::lock_guard lock(mutex_);
  auto& candidates = records_[record->id];
  if (!record->multiple) {
    const auto conflict = std::ranges::find_if(candidates, [](const auto& item) {
      return item->available.load(std::memory_order_acquire);
    });
    if (conflict != candidates.end()) {
      throw tokmon::Error("arche.capability.conflict",
                          "capability already has an active provider: " +
                              record->id);
    }
  }
  candidates.push_back(record);
  return record;
}

void CapabilityRegistry::erase(
    const std::shared_ptr<CapabilityRecord>& record) {
  std::lock_guard lock(mutex_);
  const auto iterator = records_.find(record->id);
  if (iterator == records_.end()) return;
  std::erase(iterator->second, record);
  if (iterator->second.empty()) records_.erase(iterator);
}

void CapabilityRegistry::set_available(
    const std::shared_ptr<CapabilityRecord>& record, bool available) {
  std::lock_guard lock(mutex_);
  if (available && !record->multiple) {
    const auto iterator = records_.find(record->id);
    if (iterator != records_.end()) {
      for (const auto& candidate : iterator->second) {
        if (candidate != record &&
            candidate->available.load(std::memory_order_acquire)) {
          throw tokmon::Error("arche.capability.conflict",
                              "capability already active: " + record->id);
        }
      }
    }
  }
  record->available.store(available, std::memory_order_release);
}

std::shared_ptr<CapabilityRecord> CapabilityRegistry::resolve(
    std::string_view id, std::string_view range,
    std::string_view interface_hash) const {
  std::lock_guard lock(mutex_);
  const auto iterator = records_.find(std::string(id));
  if (iterator == records_.end()) return {};
  for (auto candidate = iterator->second.rbegin();
       candidate != iterator->second.rend(); ++candidate) {
    if ((*candidate)->available.load(std::memory_order_acquire) &&
        version_satisfies((*candidate)->version, range) &&
        (interface_hash.empty() ||
         (*candidate)->interface_hash == interface_hash)) {
      return *candidate;
    }
  }
  return {};
}

std::vector<std::shared_ptr<CapabilityRecord>>
CapabilityRegistry::by_provider(const FiberId& provider) const {
  std::lock_guard lock(mutex_);
  std::vector<std::shared_ptr<CapabilityRecord>> result;
  for (const auto& [_, values] : records_) {
    for (const auto& value : values) {
      if (value->provider == provider) result.push_back(value);
    }
  }
  return result;
}

Json CapabilityRegistry::inspect() const {
  std::lock_guard lock(mutex_);
  Json result = Json::array();
  for (const auto& [id, values] : records_) {
    for (const auto& value : values) {
      result.push_back(
          {{"id", id},
           {"version", value->version},
           {"interface_hash", value->interface_hash},
           {"provider", value->provider.str()},
           {"available", value->available.load(std::memory_order_acquire)},
           {"leases", value.use_count() - 1}});
    }
  }
  return result;
}

} // namespace arche::detail

namespace arche {
namespace {

bool rule_matches(std::string_view rule, std::string_view capability) {
  return rule == capability ||
         (rule.ends_with('*') &&
          capability.starts_with(rule.substr(0, rule.size() - 1)));
}

} // namespace

Context::Context(ContextId id, std::string realm,
                 std::shared_ptr<detail::CapabilityRegistry> registry,
                 std::shared_ptr<Context> parent)
    : id_(std::move(id)), realm_(std::move(realm)),
      registry_(std::move(registry)), parent_(std::move(parent)) {}

std::shared_ptr<Context> Context::derive(std::string realm) const {
  return std::make_shared<Context>(
      ContextId(tokmon::make_uuid()), std::move(realm), registry_,
      const_cast<Context*>(this)->shared_from_this());
}

void Context::configure_owner(FiberId fiber,
                              const PluginDescriptor& descriptor,
                              Json config) {
  std::lock_guard lock(policy_mutex_);
  owner_fiber_ = std::move(fiber);
  owner_descriptor_ = descriptor;
  config_ = std::move(config);
}

void Context::deny(std::string capability) {
  const auto key = capability;
  bool inserted = false;
  {
    std::lock_guard lock(policy_mutex_);
    inserted = denied_.insert(std::move(capability)).second;
  }
  if (inserted && ledger_) {
    ledger_->add("deny:" + key, [self = shared_from_this(), key] {
      std::lock_guard lock(self->policy_mutex_);
      self->denied_.erase(key);
    });
  }
}

void Context::isolate(std::string capability, std::string realm) {
  if (capability.empty() || realm.empty()) {
    throw tokmon::Error("arche.realm.invalid",
                        "capability and realm must not be empty");
  }
  const auto key = capability;
  std::optional<std::string> previous;
  {
    std::lock_guard lock(policy_mutex_);
    if (const auto found = isolation_.find(key); found != isolation_.end())
      previous = found->second;
    isolation_[key] = std::move(realm);
  }
  if (ledger_) {
    ledger_->add("isolate:" + key,
                 [self = shared_from_this(), key, previous = std::move(previous)] {
                   std::lock_guard lock(self->policy_mutex_);
                   if (previous) self->isolation_[key] = *previous;
                   else self->isolation_.erase(key);
                 });
  }
}

void Context::intercept(std::string capability,
                        CapabilityInterceptor interceptor) {
  if (capability.empty() || !interceptor) {
    throw tokmon::Error("arche.interceptor.invalid",
                        "interceptor requires a capability and callback");
  }
  const auto key = capability;
  std::optional<CapabilityInterceptor> previous;
  {
    std::lock_guard lock(policy_mutex_);
    if (const auto found = interceptors_.find(key); found != interceptors_.end())
      previous = found->second;
    interceptors_[key] = std::move(interceptor);
  }
  if (ledger_) {
    ledger_->add("intercept:" + key,
                 [self = shared_from_this(), key,
                  previous = std::move(previous)]() mutable {
                   std::lock_guard lock(self->policy_mutex_);
                   if (previous) self->interceptors_[key] = std::move(*previous);
                   else self->interceptors_.erase(key);
                 });
  }
}

CapabilityDecision Context::local_decision(
    std::string_view capability) const {
  std::lock_guard lock(policy_mutex_);
  if (std::ranges::any_of(denied_, [&](const auto& rule) {
        return rule_matches(rule, capability);
      })) {
    return {false, "denied by context " + id_.str()};
  }
  for (const auto& [rule, interceptor] : interceptors_) {
    if (!rule_matches(rule, capability)) continue;
    try {
      auto decision = interceptor(capability);
      if (!decision.allowed) return decision;
    } catch (const std::exception& error) {
      return {false, "interceptor failed closed: " + std::string(error.what())};
    } catch (...) {
      return {false, "interceptor failed closed"};
    }
  }
  return {};
}

bool Context::permits(std::string_view capability) const {
  if (!local_decision(capability).allowed) return false;
  if (const auto parent = parent_.lock()) return parent->permits(capability);
  return true;
}

Json Context::explain_resolution(std::string_view capability,
                                 std::string_view range) const {
  Json chain = Json::array();
  auto cursor = std::const_pointer_cast<const Context>(shared_from_this());
  while (cursor) {
    const auto decision = cursor->local_decision(capability);
    chain.push_back({{"context_id", cursor->id().str()},
                     {"realm", cursor->realm()},
                     {"allowed", decision.allowed},
                     {"reason", decision.reason}});
    if (!decision.allowed) break;
    cursor = cursor->parent();
  }
  const auto qualified = qualify_lookup(capability);
  const auto declared = find_requirement(capability);
  const auto record = permits(capability)
                          ? registry_->resolve(
                                qualified, range,
                                declared ? declared->interface_hash
                                         : std::string_view{})
                          : nullptr;
  return {{"requested", capability},
          {"range", range},
          {"qualified", qualified},
          {"policy_chain", std::move(chain)},
          {"resolved", record != nullptr},
          {"provider", record ? Json(record->provider.str()) : Json(nullptr)},
          {"version", record ? Json(record->version) : Json(nullptr)},
          {"interface_hash",
           record ? Json(record->interface_hash) : Json(nullptr)}};
}

void Context::bind_activation(
    FiberId fiber, const PluginDescriptor* descriptor, EffectLedger* ledger,
    std::vector<std::shared_ptr<detail::CapabilityRecord>>*
        staged_provisions) {
  if (ledger_ != nullptr) {
    throw tokmon::Error("arche.context.activation",
                        "context already has an active apply transaction");
  }
  if (owner_descriptor_ && fiber != owner_fiber_) {
    throw tokmon::Error("arche.context.owner",
                        "activation fiber does not own this context");
  }
  active_fiber_ = std::move(fiber);
  active_descriptor_ = descriptor;
  ledger_ = ledger;
  staged_provisions_ = staged_provisions;
}

void Context::clear_activation() noexcept {
  active_fiber_ = FiberId{};
  active_descriptor_ = nullptr;
  ledger_ = nullptr;
  staged_provisions_ = nullptr;
}

void Context::provide_json(std::string id, std::string version, Json value) {
  provide<Json>(std::move(id), std::move(version),
                std::make_shared<Json>(std::move(value)));
}

std::optional<FiberId> Context::resolve_provider(
    const CapabilityRequirement& requirement) const {
  if (!permits(requirement.capability)) return std::nullopt;
  if (const auto record = registry_->resolve(
          qualify_lookup(requirement.capability), requirement.range,
          requirement.interface_hash)) {
    return record->provider;
  }
  return std::nullopt;
}

void Context::on_unload(std::string label, Undo undo) {
  ensure_activation();
  ledger_->add(std::move(label), std::move(undo));
}

Json Context::inspect() const {
  Json denied = Json::array();
  Json isolation = Json::object();
  Json interceptors = Json::array();
  {
    std::lock_guard lock(policy_mutex_);
    for (const auto& value : denied_) denied.push_back(value);
    for (const auto& [key, value] : isolation_) isolation[key] = value;
    for (const auto& [key, _] : interceptors_) interceptors.push_back(key);
  }
  return {{"id", id_.str()},
          {"realm", realm_},
          {"parent", parent_.expired() ? Json(nullptr)
                                       : Json(parent_.lock()->id().str())},
          {"owner_fiber", owner_fiber_.empty() ? Json(nullptr)
                                                : Json(owner_fiber_.str())},
          {"denied", denied},
          {"isolation", isolation},
          {"interceptors", interceptors},
          {"capabilities", registry_->inspect()}};
}

void Context::ensure_activation() const {
  if (!ledger_ || !active_descriptor_ || !staged_provisions_) {
    throw tokmon::Error("arche.context.outside_apply",
                        "effect registration is only valid during apply");
  }
}

const CapabilityProvision* Context::find_provision(std::string_view id) const {
  const auto* descriptor = active_descriptor_
                               ? active_descriptor_
                               : (owner_descriptor_ ? &*owner_descriptor_ : nullptr);
  if (!descriptor) return nullptr;
  const auto iterator =
      std::ranges::find(descriptor->provides, id,
                        &CapabilityProvision::capability);
  return iterator == descriptor->provides.end() ? nullptr : &*iterator;
}

const CapabilityRequirement* Context::find_requirement(
    std::string_view id) const {
  const auto* descriptor = active_descriptor_
                               ? active_descriptor_
                               : (owner_descriptor_ ? &*owner_descriptor_ : nullptr);
  if (!descriptor) return nullptr;
  const auto iterator =
      std::ranges::find(descriptor->requirements, id,
                        &CapabilityRequirement::capability);
  return iterator == descriptor->requirements.end() ? nullptr : &*iterator;
}

void Context::ensure_declared_provision(std::string_view id) const {
  if (!find_provision(id)) {
    throw tokmon::Error("arche.capability.undeclared_provision",
                        "plugin did not declare provision: " + std::string(id));
  }
}

void Context::ensure_declared_requirement(std::string_view id) const {
  if (!active_descriptor_ && !owner_descriptor_) return;
  if (!find_requirement(id)) {
    throw tokmon::Error("arche.capability.undeclared_requirement",
                        "plugin did not declare requirement: " +
                            std::string(id));
  }
}

std::optional<std::string> Context::isolated_realm(
    std::string_view id) const {
  {
    std::lock_guard lock(policy_mutex_);
    if (const auto exact = isolation_.find(id); exact != isolation_.end())
      return exact->second;
    for (const auto& [rule, realm] : isolation_) {
      if (rule_matches(rule, id)) return realm;
    }
  }
  if (const auto parent = parent_.lock()) return parent->isolated_realm(id);
  return std::nullopt;
}

std::string Context::qualify(std::string id) const {
  if (id.starts_with("realm:")) return id;
  if (const auto isolated = isolated_realm(id))
    return "realm:" + *isolated + ":" + id;
  return id;
}

std::string Context::qualify_lookup(std::string_view id) const {
  return qualify(std::string(id));
}

} // namespace arche
