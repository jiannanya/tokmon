#include <tokmon/workbench_document.hpp>

#include <tokmon/common/types.hpp>

#include <workbench_document_data.hpp>

namespace tokmon::desktop {
namespace {

white::Rect region(white::DeclarativeView &view, std::string_view id,
                   bool required = true) {
  if (const auto *node = view.find_by_id(id))
    return node->layout();
  if (required)
    throw tokmon::Error("tokmon.workbench.region",
                        "declarative region is missing: " + std::string(id));
  return {};
}

} // namespace

WorkbenchDocument::WorkbenchDocument()
    : view_(white::DeclarativeView::parse(detail::workbench_document_source)) {}

WorkbenchRegions WorkbenchDocument::layout(float width, float height,
                                           const WorkbenchShellState &state) {
  view_->set_state(
      {{"sidebar",
        {{"visible", state.sidebar_visible}, {"width", state.sidebar_width}}},
       {"viewer",
        {{"visible", state.viewer_visible},
         {"width", state.viewer_width},
         {"explorerWidth", state.explorer_width}}}});
  view_->layout(width, height);
  WorkbenchRegions result;
  result.menu_bar = region(*view_, "menu-bar");
  result.sidebar = region(*view_, "sidebar", false);
  result.conversation = region(*view_, "conversation");
  result.conversation_header = region(*view_, "conversation-header");
  result.timeline = region(*view_, "timeline");
  result.composer = region(*view_, "composer");
  result.viewer = region(*view_, "viewer", false);
  result.viewer_header = region(*view_, "viewer-header", false);
  result.document = region(*view_, "document", false);
  result.explorer = region(*view_, "explorer", false);
  return result;
}

const white::ViewBlueprint &WorkbenchDocument::blueprint() const noexcept {
  return view_->blueprint();
}

std::string_view WorkbenchDocument::source() noexcept {
  return detail::workbench_document_source;
}

} // namespace tokmon::desktop
