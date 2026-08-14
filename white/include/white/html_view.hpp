#pragma once

#include <white/native_component.hpp>
#include <white/renderer.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string_view>

namespace white {

// Primary HTML/CSS integration surface. The DOM and component instances are
// retained across frames; product state is immutable JSON and native component
// boundaries are resolved through the composed registry.
class HtmlView final {
public:
  HtmlView(std::string_view html, std::string_view css,
           std::shared_ptr<NativeComponentRegistry> registry = {});

  void set_state(tokmon::Json state);
  void layout(float width, float height);
  void render(RasterSurface &surface);
  [[nodiscard]] bool dispatch(UiEvent event);
  [[nodiscard]] bool pointer_move(float x, float y);
  [[nodiscard]] bool scroll(float x, float y, float delta_y);

  [[nodiscard]] Document &document() noexcept { return document_; }
  [[nodiscard]] const Document &document() const noexcept { return document_; }
  [[nodiscard]] Node *find_by_id(std::string_view id) {
    return document_.find_by_id(id);
  }

private:
  struct MountedComponent {
    Node *node{nullptr};
    std::string id;
    std::unique_ptr<NativeComponent> instance;
  };

  void mount_components();
  void update_components();
  [[nodiscard]] MountedComponent *component_for(Node *node) noexcept;

  Document document_;
  std::shared_ptr<NativeComponentRegistry> registry_;
  tokmon::Json state_{tokmon::Json::object()};
  std::vector<MountedComponent> components_;
  std::uint64_t state_revision_{0};
  std::uint64_t updated_state_revision_{static_cast<std::uint64_t>(-1)};
  std::uint64_t mounted_registry_revision_{0};
  float viewport_width_{0};
  float viewport_height_{0};
};

} // namespace white
