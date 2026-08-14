#include <tokmon/workbench_boundaries.hpp>

#include <white/native_component.hpp>

#include <array>
#include <string>
#include <string_view>

namespace tokmon::desktop {
namespace {

struct BoundaryDescriptor {
  std::string_view id;
  std::string_view role;
};

constexpr std::array boundaries{
    BoundaryDescriptor{"tokmon.menu-bar", "menubar"},
    BoundaryDescriptor{"tokmon.sidebar", "navigation"},
    BoundaryDescriptor{"tokmon.conversation", "main"},
    BoundaryDescriptor{"tokmon.workspace", "complementary"},
};

class WorkbenchBoundary final : public white::NativeComponent {
public:
  explicit WorkbenchBoundary(std::string role) : role_(std::move(role)) {}

  void mount(white::Node &node) override {
    node.add_class("native-component");
    node.set_attribute("role", role_);
    node.set_attribute("data-mounted", "true");
  }

private:
  std::string role_;
};

} // namespace

void register_workbench_boundaries(white::NativeComponentRegistry &registry) {
  for (const auto &boundary : boundaries) {
    registry.register_factory(
        std::string(boundary.id),
        [role = std::string(boundary.role)] {
          return std::make_unique<WorkbenchBoundary>(role);
        });
  }
}

void unregister_workbench_boundaries(white::NativeComponentRegistry &registry) {
  for (const auto &boundary : boundaries)
    registry.unregister_factory(boundary.id);
}

} // namespace tokmon::desktop
