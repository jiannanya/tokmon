#pragma once

#include <white/html_view.hpp>

#include <memory>
#include <string_view>

namespace tokmon::desktop {

struct WorkbenchShellState {
  float sidebar_width{0};
  float viewer_width{0};
  float explorer_width{0};
  bool sidebar_visible{true};
  bool viewer_visible{true};
  friend bool operator==(const WorkbenchShellState&,
                         const WorkbenchShellState&) = default;
};

struct WorkbenchRegions {
  white::Rect menu_bar;
  white::Rect sidebar;
  white::Rect conversation;
  white::Rect conversation_header;
  white::Rect timeline;
  white::Rect composer;
  white::Rect viewer;
  white::Rect viewer_header;
  white::Rect document;
  white::Rect explorer;
};

// Tokmon's product shell is parsed once from HTML and CSS. White owns the
// retained DOM, cascade and Yoga layout; data-native nodes are high-performance
// product components supplied by Tokmon plugins.
class WorkbenchDocument final {
public:
  explicit WorkbenchDocument(
      std::shared_ptr<white::NativeComponentRegistry> native_components = {});

  [[nodiscard]] WorkbenchRegions layout(float width, float height,
                                        const WorkbenchShellState &state);
  void render(white::RasterSurface &surface);
  void invalidate(white::Rect damage = {});
  [[nodiscard]] white::Document &document() noexcept {
    return view_->document();
  }
  [[nodiscard]] const white::Document &document() const noexcept {
    return view_->document();
  }
  [[nodiscard]] static std::string_view html_source() noexcept;
  [[nodiscard]] static std::string_view css_source() noexcept;

private:
  std::unique_ptr<white::HtmlView> view_;
  WorkbenchShellState state_;
  float viewport_width_{0};
  float viewport_height_{0};
};

} // namespace tokmon::desktop
