#pragma once

#include <white/document.hpp>
#include <white/retained.hpp>

#include <tokmon/common/types.hpp>

#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace white {

class RasterSurface;

class NativeComponent {
public:
  virtual ~NativeComponent() = default;
  virtual void mount(Node &) {}
  virtual void update(Node &, const tokmon::Json &) {}
  virtual void paint(RasterSurface &, const Node &, const DamageRegion &) {}
  [[nodiscard]] virtual bool dispatch(Node &, UiEvent &) { return false; }
};

class NativeComponentRegistry final {
public:
  using Factory = std::function<std::unique_ptr<NativeComponent>()>;

  void register_factory(std::string id, Factory factory);
  void unregister_factory(std::string_view id);
  [[nodiscard]] bool contains(std::string_view id) const noexcept;
  [[nodiscard]] std::unique_ptr<NativeComponent>
  create(std::string_view id) const;
  [[nodiscard]] std::vector<std::string> ids() const;
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
  std::map<std::string, Factory, std::less<>> factories_;
  std::uint64_t revision_{0};
};

} // namespace white
