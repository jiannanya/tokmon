#pragma once

#include <white/declarative.hpp>

#include <memory>
#include <string_view>

namespace tokmon::desktop {

struct WorkbenchShellState {
  float sidebar_width{0};
  float viewer_width{0};
  float explorer_width{0};
  bool sidebar_visible{true};
  bool viewer_visible{true};
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

// Tokmon's product shell is a White UI document. Responsive policy is supplied
// as immutable state; White owns component expansion and structural layout.
class WorkbenchDocument final {
public:
  WorkbenchDocument();

  [[nodiscard]] WorkbenchRegions layout(float width, float height,
                                        const WorkbenchShellState &state);
  [[nodiscard]] const white::ViewBlueprint &blueprint() const noexcept;
  [[nodiscard]] static std::string_view source() noexcept;

private:
  std::unique_ptr<white::DeclarativeView> view_;
};

} // namespace tokmon::desktop
