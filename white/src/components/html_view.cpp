#include <white/html_view.hpp>

#include <algorithm>

namespace white {
namespace {

void collect_native_nodes(Node &node, std::vector<Node *> &result) {
  if (!node.attribute("data-native").empty()) result.push_back(&node);
  for (auto &child : node.children()) collect_native_nodes(*child, result);
}

} // namespace

HtmlView::HtmlView(std::string_view html, std::string_view css,
                   std::shared_ptr<NativeComponentRegistry> registry)
    : document_(Document::parse_html(html, css)),
      registry_(registry ? std::move(registry)
                         : std::make_shared<NativeComponentRegistry>()) {
  mount_components();
}

void HtmlView::set_state(tokmon::Json state) {
  if (state == state_) return;
  state_ = std::move(state);
  ++state_revision_;
  document_.invalidate(Invalidation::paint);
}

void HtmlView::layout(float width, float height) {
  viewport_width_ = width;
  viewport_height_ = height;
  document_.layout(width, height);
  update_components();
}

void HtmlView::render(RasterSurface &surface) {
  if (viewport_width_ != static_cast<float>(surface.width()) ||
      viewport_height_ != static_cast<float>(surface.height()))
    layout(static_cast<float>(surface.width()),
           static_cast<float>(surface.height()));
  else
    update_components();
  const auto damage = document_.damage_since(surface.document_revision(document_));
  if (damage.empty()) return;
  surface.render(document_);
  for (auto &component : components_)
    component.instance->paint(surface, *component.node, damage);
}

bool HtmlView::dispatch(UiEvent event) {
  auto *target = document_.hit_test(event.x, event.y);
  bool changed = false;
  for (auto *node = target; node != nullptr; node = node->parent()) {
    if (auto *component = component_for(node)) {
      changed = component->instance->dispatch(*node, event) || changed;
      break;
    }
  }
  document_.dispatch(std::move(event));
  if (changed) document_.invalidate(Invalidation::paint);
  return changed;
}

bool HtmlView::pointer_move(float x, float y) {
  const auto revision = document_.revision();
  document_.pointer_move(x, y);
  return revision != document_.revision();
}

bool HtmlView::scroll(float x, float y, float delta_y) {
  const auto revision = document_.revision();
  document_.scroll(x, y, delta_y);
  return revision != document_.revision();
}

void HtmlView::mount_components() {
  components_.clear();
  std::vector<Node *> nodes;
  collect_native_nodes(document_.root(), nodes);
  for (auto *node : nodes) {
    auto id = node->attribute("data-native");
    auto instance = registry_->create(id);
    if (!instance) continue;
    instance->mount(*node);
    components_.push_back({node, std::move(id), std::move(instance)});
  }
  mounted_registry_revision_ = registry_->revision();
  updated_state_revision_ = static_cast<std::uint64_t>(-1);
}

void HtmlView::update_components() {
  if (mounted_registry_revision_ != registry_->revision()) mount_components();
  if (updated_state_revision_ == state_revision_) return;
  for (auto &component : components_)
    component.instance->update(*component.node, state_);
  updated_state_revision_ = state_revision_;
}

HtmlView::MountedComponent *HtmlView::component_for(Node *node) noexcept {
  const auto found = std::ranges::find(components_, node,
                                       &MountedComponent::node);
  return found == components_.end() ? nullptr : &*found;
}

} // namespace white
