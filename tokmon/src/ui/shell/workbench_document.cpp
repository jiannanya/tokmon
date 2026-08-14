#include <tokmon/workbench_document.hpp>

#include <tokmon/common/types.hpp>

#include <workbench_document_data.hpp>

#include <algorithm>
#include <string>

namespace tokmon::desktop {
namespace {

white::Rect region(white::Document &document, std::string_view id,
                   bool required = true) {
  if (const auto *node = document.find_by_id(id))
    return node->layout();
  if (required)
    throw tokmon::Error("tokmon.workbench.region",
                        "declarative region is missing: " + std::string(id));
  return {};
}

std::string dimension_style(float width, bool visible) {
  return "width:" + std::to_string(visible ? std::max(0.0F, width) : 0.0F) +
         "px;min-width:0px;overflow:hidden";
}

} // namespace

WorkbenchDocument::WorkbenchDocument(
    std::shared_ptr<white::NativeComponentRegistry> native_components)
    : view_(std::make_unique<white::HtmlView>(detail::workbench_html,
                                              detail::workbench_css,
                                              std::move(native_components))) {}

WorkbenchRegions WorkbenchDocument::layout(float width, float height,
                                           const WorkbenchShellState &state) {
  const bool changed = state_ != state || viewport_width_ != width ||
                       viewport_height_ != height;
  state_ = state;
  viewport_width_ = width;
  viewport_height_ = height;
  auto &document = view_->document();
  if (changed) {
    document.find_by_id("sidebar")->set_attribute(
        "style", dimension_style(state.sidebar_width, state.sidebar_visible));
    document.find_by_id("viewer")->set_attribute(
        "style", dimension_style(state.viewer_width, state.viewer_visible));
    document.find_by_id("explorer")->set_attribute(
        "style", dimension_style(state.explorer_width, state.viewer_visible));
    document.invalidate(white::Invalidation::style |
                        white::Invalidation::layout |
                        white::Invalidation::paint);
  }
  view_->layout(width, height);
  WorkbenchRegions result;
  result.menu_bar = region(document, "menu-bar");
  result.sidebar = state.sidebar_visible ? region(document, "sidebar")
                                         : white::Rect{};
  result.conversation = region(document, "conversation");
  result.conversation_header = region(document, "conversation-header");
  result.timeline = region(document, "timeline");
  result.composer = region(document, "composer");
  result.viewer = state.viewer_visible ? region(document, "viewer")
                                       : white::Rect{};
  result.viewer_header = state.viewer_visible
                             ? region(document, "viewer-header")
                             : white::Rect{};
  result.document = state.viewer_visible ? region(document, "document")
                                         : white::Rect{};
  result.explorer = state.viewer_visible ? region(document, "explorer")
                                         : white::Rect{};
  return result;
}

void WorkbenchDocument::render(white::RasterSurface &surface) {
  if (viewport_width_ != static_cast<float>(surface.width()) ||
      viewport_height_ != static_cast<float>(surface.height()))
    (void)layout(static_cast<float>(surface.width()),
                 static_cast<float>(surface.height()), state_);
  view_->render(surface);
}

void WorkbenchDocument::invalidate(white::Rect damage) {
  view_->document().invalidate(white::Invalidation::paint, damage);
}

std::string_view WorkbenchDocument::html_source() noexcept {
  return detail::workbench_html;
}

std::string_view WorkbenchDocument::css_source() noexcept {
  return detail::workbench_css;
}

} // namespace tokmon::desktop
