#include <white/native_component.hpp>

#include <tokmon/common/types.hpp>

namespace white {

void NativeComponentRegistry::register_factory(std::string id,
                                                Factory factory) {
  if (id.empty() || !factory)
    throw tokmon::Error("white.native-component.register",
                        "component id and factory are required");
  factories_.insert_or_assign(std::move(id), std::move(factory));
  ++revision_;
}

void NativeComponentRegistry::unregister_factory(std::string_view id) {
  if (factories_.erase(id) > 0) ++revision_;
}

bool NativeComponentRegistry::contains(std::string_view id) const noexcept {
  return factories_.contains(id);
}

std::unique_ptr<NativeComponent>
NativeComponentRegistry::create(std::string_view id) const {
  const auto found = factories_.find(id);
  return found == factories_.end() ? nullptr : found->second();
}

std::vector<std::string> NativeComponentRegistry::ids() const {
  std::vector<std::string> result;
  result.reserve(factories_.size());
  for (const auto &[id, factory] : factories_) {
    (void)factory;
    result.push_back(id);
  }
  return result;
}

} // namespace white
