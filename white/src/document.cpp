#include <white/document.hpp>

#include <tokmon/common/types.hpp>

#include <lexbor/html/html.h>
#include <lexbor/dom/interface.h>
#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/element.h>
#include <yoga/Yoga.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <regex>
#include <sstream>
#include <stack>
#include <tuple>
#include <set>

namespace white {
namespace {

std::string trim(std::string value) {
  const auto first =
      std::find_if_not(value.begin(), value.end(),
                       [](unsigned char c) { return std::isspace(c); });
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(),
                       [](unsigned char c) { return std::isspace(c); })
          .base();
  return first < last ? std::string(first, last) : std::string{};
}

float number(std::string_view value, float fallback = 0) {
  std::string owned(value);
  if (owned.ends_with("px")) {
    owned.resize(owned.size() - 2);
  }
  try {
    return std::stof(owned);
  } catch (...) {
    return fallback;
  }
}

void apply_property(Style& style, std::string_view name,
                    std::string_view value) {
  if (name == "width") {
    style.width = number(value);
  } else if (name == "height") {
    style.height = number(value);
  } else if (name == "flex-grow") {
    style.flex_grow = number(value);
  } else if (name == "flex-direction") {
    style.flex_direction =
        value == "row" ? FlexDirection::row : FlexDirection::column;
  } else if (name == "align-items") {
    if (value == "center") style.align_items = Align::center;
    else if (value == "flex-end") style.align_items = Align::end;
    else if (value == "flex-start") style.align_items = Align::start;
    else style.align_items = Align::stretch;
  } else if (name == "gap") {
    style.gap = number(value);
  } else if (name == "padding") {
    style.padding = number(value);
  } else if (name == "margin") {
    style.margin = number(value);
  } else if (name == "border-width") {
    style.border_width = number(value);
  } else if (name == "border-radius") {
    style.border_radius = number(value);
  } else if (name == "font-size") {
    style.font_size = number(value, 16);
  } else if (name == "line-height") {
    style.line_height = number(value, 1.35F);
  } else if (name == "font-weight") {
    style.font_weight = static_cast<int>(number(value, 400));
  } else if (name == "opacity") {
    style.opacity = std::clamp(number(value, 1), 0.0F, 1.0F);
  } else if (name == "direction") {
    style.text_direction = value == "rtl" ? TextDirection::right_to_left
                           : value == "ltr" ? TextDirection::left_to_right
                                            : TextDirection::automatic;
  } else if (name == "color") {
    style.color = Color::parse(value);
  } else if (name == "background" || name == "background-color") {
    style.background = Color::parse(value);
  } else if (name == "border-color") {
    style.border_color = Color::parse(value);
  } else if (name == "overflow") {
    style.overflow = value == "hidden" ? Overflow::hidden
                     : value == "scroll" ? Overflow::scroll
                     : value == "auto" ? Overflow::automatic
                                        : Overflow::visible;
  }
}

void parse_declarations(std::string_view text, Style& style) {
  std::stringstream input{std::string(text)};
  std::string declaration;
  while (std::getline(input, declaration, ';')) {
    const auto split = declaration.find(':');
    if (split == std::string::npos) continue;
    apply_property(style, trim(declaration.substr(0, split)),
                   trim(declaration.substr(split + 1)));
  }
}

bool matches_compound(const Node& node, std::string_view selector) {
  bool require_hover = false;
  bool require_focus = false;
  if (const auto pseudo = selector.find(':'); pseudo != std::string_view::npos) {
    const auto value = selector.substr(pseudo + 1);
    require_hover = value == "hover";
    require_focus = value == "focus";
    selector = selector.substr(0, pseudo);
  }
  if (require_hover && !node.hovered()) return false;
  if (require_focus && !node.focused()) return false;
  std::size_t position = 0;
  if (!selector.empty() && selector.front() != '#' && selector.front() != '.') {
    const auto end = selector.find_first_of("#.");
    if (node.tag() != selector.substr(0, end)) return false;
    position = end == std::string_view::npos ? selector.size() : end;
  }
  while (position < selector.size()) {
    const auto marker = selector[position++];
    const auto end = selector.find_first_of("#.", position);
    const auto value = selector.substr(position, end - position);
    if (marker == '#' && node.id() != value) return false;
    if (marker == '.' &&
        std::ranges::find(node.classes(), value) == node.classes().end())
      return false;
    position = end == std::string_view::npos ? selector.size() : end;
  }
  return !selector.empty() || require_hover || require_focus;
}

bool matches(const Node& node, std::string_view selector) {
  std::vector<std::string> parts;
  std::stringstream input{std::string(selector)};
  std::string part;
  while (input >> part) parts.push_back(std::move(part));
  if (parts.empty() || !matches_compound(node, parts.back())) return false;
  auto* ancestor = node.parent();
  for (auto index = parts.size() - 1; index > 0; --index) {
    while (ancestor && !matches_compound(*ancestor, parts[index - 1]))
      ancestor = ancestor->parent();
    if (!ancestor) return false;
    ancestor = ancestor->parent();
  }
  return true;
}

void walk(Node& node, const std::function<void(Node&)>& visitor) {
  visitor(node);
  for (auto& child : node.children()) {
    walk(*child, visitor);
  }
}

YGAlign yoga_align(Align align) {
  switch (align) {
  case Align::start:
    return YGAlignFlexStart;
  case Align::center:
    return YGAlignCenter;
  case Align::end:
    return YGAlignFlexEnd;
  case Align::stretch:
    return YGAlignStretch;
  }
  return YGAlignStretch;
}

YGSize measure_text(YGNodeConstRef node, float width,
                    YGMeasureMode width_mode, float,
                    YGMeasureMode) {
  const auto* value = static_cast<const Node*>(YGNodeGetContext(node));
  float units = 0;
  for (std::size_t index = 0; index < value->text().size();) {
    const auto byte = static_cast<unsigned char>(value->text()[index]);
    std::size_t length = byte < 0x80U ? 1U : (byte < 0xe0U ? 2U :
                         (byte < 0xf0U ? 3U : 4U));
    units += length == 1U ? 0.58F : 1.0F;
    index += std::min(length, value->text().size() - index);
  }
  const auto natural_width = units * value->style().font_size;
  const auto line_height =
      value->style().font_size * value->style().line_height;
  const auto measured_width =
      width_mode == YGMeasureModeUndefined ? natural_width
                                           : std::min(width, natural_width);
  const auto lines =
      measured_width > 0
          ? std::max(1.0F, std::ceil(natural_width / measured_width))
          : 1.0F;
  return {measured_width, line_height * lines};
}

YGNodeRef build_yoga(Node& node, std::vector<YGNodeRef>& nodes) {
  auto yoga = YGNodeNew();
  nodes.push_back(yoga);
  YGNodeSetContext(yoga, &node);
  const auto& style = node.style();
  if (style.width) YGNodeStyleSetWidth(yoga, *style.width);
  if (style.height) YGNodeStyleSetHeight(yoga, *style.height);
  YGNodeStyleSetFlexGrow(yoga, style.flex_grow);
  YGNodeStyleSetFlexDirection(
      yoga, style.flex_direction == FlexDirection::row ? YGFlexDirectionRow
                                                       : YGFlexDirectionColumn);
  YGNodeStyleSetAlignItems(yoga, yoga_align(style.align_items));
  YGNodeStyleSetPadding(yoga, YGEdgeAll, style.padding);
  YGNodeStyleSetMargin(yoga, YGEdgeAll, style.margin);
  YGNodeStyleSetGap(yoga, YGGutterAll, style.gap);
  if (style.overflow == Overflow::hidden)
    YGNodeStyleSetOverflow(yoga, YGOverflowHidden);
  else if (style.overflow == Overflow::scroll ||
           style.overflow == Overflow::automatic)
    YGNodeStyleSetOverflow(yoga, YGOverflowScroll);
  if (!node.text().empty() && node.children().empty()) {
    YGNodeSetMeasureFunc(yoga, &measure_text);
  }
  std::size_t index = 0;
  for (auto& child : node.children()) {
    YGNodeInsertChild(yoga, build_yoga(*child, nodes), index++);
  }
  return yoga;
}

void copy_layout(Node& node, YGNodeRef yoga, float parent_x,
                 float parent_y) {
  node.layout() = {
      parent_x + YGNodeLayoutGetLeft(yoga),
      parent_y + YGNodeLayoutGetTop(yoga),
      YGNodeLayoutGetWidth(yoga),
      YGNodeLayoutGetHeight(yoga)};
  float content_height = node.layout().height;
  for (std::size_t index = 0; index < node.children().size(); ++index) {
    const auto child = YGNodeGetChild(yoga, index);
    content_height = std::max(
        content_height,
        YGNodeLayoutGetTop(child) + YGNodeLayoutGetHeight(child) +
            node.style().padding);
  }
  node.set_scroll_metrics(content_height);
  for (std::size_t index = 0; index < node.children().size(); ++index) {
    copy_layout(*node.children()[index], YGNodeGetChild(yoga, index),
                node.layout().x,
                node.layout().y - node.scroll_offset_y());
  }
}

Node* find_hit(Node& node, float x, float y) {
  const bool inside = node.layout().contains(x, y);
  if (!inside && node.style().overflow != Overflow::visible) return nullptr;
  for (auto iterator = node.children().rbegin();
       iterator != node.children().rend(); ++iterator) {
    if (auto* result = find_hit(**iterator, x, y)) return result;
  }
  return inside ? &node : nullptr;
}

Node* find_id(Node& node, std::string_view id) {
  if (node.id() == id) return &node;
  for (auto& child : node.children()) {
    if (auto* result = find_id(*child, id)) return result;
  }
  return nullptr;
}

void focusable_nodes(Node& node, std::vector<Node*>& result) {
  if (node.focusable() && !node.disabled()) result.push_back(&node);
  for (auto& child : node.children()) focusable_nodes(*child, result);
}

tokmon::Json accessible(const Node& node) {
  tokmon::Json children = tokmon::Json::array();
  for (const auto& child : node.children()) children.push_back(accessible(*child));
  return {{"role", node.accessible_role()},
          {"name", node.accessible_name()},
          {"focused", node.focused()},
          {"disabled", node.disabled()},
          {"bounds", {{"x", node.layout().x}, {"y", node.layout().y},
                      {"width", node.layout().width},
                      {"height", node.layout().height}}},
          {"children", std::move(children)}};
}

void convert_lexbor_children(lxb_dom_node_t* source, Node& target) {
  for (auto* child = lxb_dom_node_first_child(source); child;
       child = lxb_dom_node_next(child)) {
    const auto type = lxb_dom_node_type(child);
    if (type == LXB_DOM_NODE_TYPE_TEXT) {
      size_t length = 0;
      const auto* text = lxb_dom_node_text_content(child, &length);
      auto value = trim(std::string(reinterpret_cast<const char*>(text), length));
      if (!value.empty()) {
        target.set_text(target.text().empty() ? std::move(value)
                                              : target.text() + " " + value);
      }
      continue;
    }
    if (type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
    auto* element = lxb_dom_interface_element(child);
    size_t name_length = 0;
    const auto* name = lxb_dom_element_qualified_name(element, &name_length);
    auto converted = std::make_unique<Node>(
        std::string(reinterpret_cast<const char*>(name), name_length));
    for (auto* attr = lxb_dom_element_first_attribute(element); attr;
         attr = lxb_dom_element_next_attribute(attr)) {
      size_t attr_name_length = 0;
      size_t attr_value_length = 0;
      const auto* attr_name =
          lxb_dom_attr_qualified_name(attr, &attr_name_length);
      const auto* attr_value = lxb_dom_attr_value(attr, &attr_value_length);
      converted->set_attribute(
          std::string(reinterpret_cast<const char*>(attr_name), attr_name_length),
          attr_value ? std::string(reinterpret_cast<const char*>(attr_value),
                                   attr_value_length)
                     : std::string{});
    }
    auto& added = target.append(std::move(converted));
    convert_lexbor_children(child, added);
  }
}

} // namespace

Color Color::parse(std::string_view value) {
  if (value == "transparent") return {0, 0, 0, 0};
  if (value == "white") return {255, 255, 255, 255};
  if (value == "black") return {0, 0, 0, 255};
  if (value == "red") return {255, 0, 0, 255};
  if (value == "blue") return {0, 0, 255, 255};
  if (value == "green") return {0, 128, 0, 255};
  if (value.starts_with('#') && (value.size() == 7 || value.size() == 9)) {
    const auto byte = [&](std::size_t offset) {
      unsigned result = 0;
      const auto part = value.substr(offset, 2);
      std::from_chars(part.data(), part.data() + part.size(), result, 16);
      return static_cast<std::uint8_t>(result);
    };
    return {byte(1), byte(3), byte(5),
            value.size() == 9 ? byte(7) : static_cast<std::uint8_t>(255)};
  }
  return {0, 0, 0, 255};
}

Node::Node(std::string tag) : tag_(std::move(tag)) {
  focusable_ = tag_ == "input" || tag_ == "textarea" || tag_ == "button" ||
               tag_ == "select" || tag_ == "a";
  editable_ = tag_ == "input" || tag_ == "textarea";
}
Node& Node::set_id(std::string id) {
  id_ = std::move(id);
  return *this;
}
Node& Node::add_class(std::string value) {
  classes_.push_back(std::move(value));
  return *this;
}
Node& Node::set_text(std::string value) {
  text_ = std::move(value);
  return *this;
}
Node& Node::set_attribute(std::string name, std::string value) {
  if (name == "id") {
    id_ = value;
  } else if (name == "class") {
    std::stringstream classes(value);
    std::string item;
    while (classes >> item) classes_.push_back(item);
  } else if (name == "style") {
    parse_declarations(value, style_);
  } else if (name == "tabindex") {
    try {
      tab_index_ = std::stoi(value);
      focusable_ = tab_index_ >= 0;
    } catch (...) {
      tab_index_ = -1;
    }
  } else if (name == "contenteditable") {
    editable_ = value.empty() || value == "true" || value == "plaintext-only";
    focusable_ = editable_;
  } else if (name == "disabled") {
    disabled_ = value.empty() || value == "true" || value == "disabled";
  }
  if (tag_ == "input" || tag_ == "textarea" || tag_ == "button" ||
      tag_ == "select" || tag_ == "a")
    focusable_ = true;
  attributes_[std::move(name)] = std::move(value);
  return *this;
}
std::string Node::attribute(std::string_view name) const {
  const auto iterator = attributes_.find(name);
  return iterator == attributes_.end() ? "" : iterator->second;
}
Node& Node::append(std::unique_ptr<Node> child) {
  child->parent_ = this;
  children_.push_back(std::move(child));
  return *children_.back();
}
Node& Node::on(std::string type, Handler handler, bool capture) {
  handlers_.emplace(std::make_pair(std::move(type), capture),
                    std::move(handler));
  return *this;
}
void Node::invoke(UiEvent& event, bool capture) {
  const auto range = handlers_.equal_range({event.type, capture});
  for (auto iterator = range.first; iterator != range.second; ++iterator) {
    iterator->second(event);
    if (event.propagation_stopped) break;
  }
}

void Node::set_scroll_metrics(float content_height) noexcept {
  content_height_ = std::max(content_height, layout_.height);
  scroll_offset_y_ = std::clamp(
      scroll_offset_y_, 0.0F,
      std::max(0.0F, content_height_ - layout_.height));
}

void Node::scroll_by(float delta) noexcept {
  if (style_.overflow != Overflow::scroll &&
      style_.overflow != Overflow::automatic)
    return;
  scroll_offset_y_ = std::clamp(
      scroll_offset_y_ + delta, 0.0F,
      std::max(0.0F, content_height_ - layout_.height));
}

std::string Node::accessible_role() const {
  if (const auto role = attribute("role"); !role.empty()) return role;
  if (tag_ == "button") return "button";
  if (tag_ == "input" || tag_ == "textarea" || editable_) return "textbox";
  if (tag_ == "a") return "link";
  if (tag_ == "img") return "image";
  if (tag_ == "ul" || tag_ == "ol") return "list";
  if (tag_ == "li") return "listitem";
  if (tag_ == "body") return "document";
  return "group";
}

std::string Node::accessible_name() const {
  if (const auto label = attribute("aria-label"); !label.empty()) return label;
  if (const auto alt = attribute("alt"); !alt.empty()) return alt;
  if (const auto value = attribute("value"); !value.empty()) return value;
  return text_;
}

StyleSheet StyleSheet::parse(std::string_view css) {
  StyleSheet result;
  static const std::regex rule(
      R"(([^{}]+)\{([^{}]*)\})", std::regex::optimize);
  const std::string owned(css);
  std::size_t order = 0;
  for (std::sregex_iterator iterator(owned.begin(), owned.end(), rule), end;
       iterator != end; ++iterator) {
    Rule value;
    value.selector = trim((*iterator)[1].str());
    value.order = order++;
    value.specificity = 1;
    value.specificity +=
        static_cast<int>(std::ranges::count(value.selector, '#')) * 100;
    value.specificity +=
        static_cast<int>(std::ranges::count(value.selector, '.')) * 10;
    value.specificity +=
        static_cast<int>(std::ranges::count(value.selector, ':')) * 10;
    std::stringstream declarations((*iterator)[2].str());
    std::string declaration;
    while (std::getline(declarations, declaration, ';')) {
      const auto split = declaration.find(':');
      if (split != std::string::npos) {
        value.declarations[trim(declaration.substr(0, split))] =
            trim(declaration.substr(split + 1));
      }
    }
    result.rules_.push_back(std::move(value));
  }
  return result;
}

void StyleSheet::apply(Node& root) const {
  auto rules = rules_;
  std::stable_sort(rules.begin(), rules.end(), [](const auto& left,
                                                  const auto& right) {
    return std::tie(left.specificity, left.order) <
           std::tie(right.specificity, right.order);
  });
  std::function<void(Node&, const Node*)> apply_node =
      [&](Node& node, const Node* parent) {
    std::set<std::string, std::less<>> explicit_properties;
    for (const auto& rule : rules) {
      bool matched = false;
      std::stringstream selectors(rule.selector);
      std::string selector;
      while (std::getline(selectors, selector, ',')) {
        if (matches(node, trim(selector))) {
          matched = true;
          break;
        }
      }
      if (!matched) continue;
      for (const auto& [name, value] : rule.declarations) {
        apply_property(node.style(), name, value);
        explicit_properties.insert(name);
      }
    }
    if (parent) {
      if (!explicit_properties.contains("color"))
        node.style().color = parent->style().color;
      if (!explicit_properties.contains("font-size"))
        node.style().font_size = parent->style().font_size;
      if (!explicit_properties.contains("font-weight"))
        node.style().font_weight = parent->style().font_weight;
      if (!explicit_properties.contains("line-height"))
        node.style().line_height = parent->style().line_height;
      if (!explicit_properties.contains("direction"))
        node.style().text_direction = parent->style().text_direction;
    }
    if (const auto inline_style = node.attribute("style"); !inline_style.empty())
      parse_declarations(inline_style, node.style());
    for (auto& child : node.children()) apply_node(*child, &node);
  };
  apply_node(root, nullptr);
}

Document::Document() : root_(std::make_unique<Node>("body")) {}

Document Document::parse_html(std::string_view html, std::string_view css) {
  auto* parsed = lxb_html_document_create();
  if (!parsed ||
      lxb_html_document_parse(
          parsed, reinterpret_cast<const lxb_char_t*>(html.data()),
          html.size()) != LXB_STATUS_OK) {
    if (parsed) lxb_html_document_destroy(parsed);
    throw tokmon::Error("white.html.parse", "Lexbor rejected HTML document");
  }
  Document result;
  result.root_ = std::make_unique<Node>("body");
  if (auto* body = lxb_html_document_body_element(parsed)) {
    convert_lexbor_children(lxb_dom_interface_node(body), *result.root_);
  }
  lxb_html_document_destroy(parsed);
  result.set_style_sheet(StyleSheet::parse(css));
  return result;
}

void Document::set_style_sheet(StyleSheet style_sheet) {
  style_sheet_ = std::move(style_sheet);
  style_sheet_->apply(*root_);
}

void Document::layout(float width, float height) {
  viewport_width_ = width;
  viewport_height_ = height;
  if (style_sheet_) style_sheet_->apply(*root_);
  root_->style().width = width;
  root_->style().height = height;
  std::vector<YGNodeRef> nodes;
  auto root = build_yoga(*root_, nodes);
  YGNodeCalculateLayout(root, width, height, YGDirectionLTR);
  copy_layout(*root_, root, 0, 0);
  YGNodeFreeRecursive(root);
}

Node* Document::hit_test(float x, float y) {
  return find_hit(*root_, x, y);
}

void Document::dispatch(UiEvent event) {
  auto* target = hit_test(event.x, event.y);
  if (!target) return;
  if (event.type == "click" && target->focusable() && !target->disabled())
    focus(target);
  std::vector<Node*> path;
  for (auto* node = target; node != nullptr; node = node->parent()) {
    path.push_back(node);
  }
  std::reverse(path.begin(), path.end());
  event.phase = EventPhase::capture;
  for (auto* node : path) {
    node->invoke(event, true);
    if (event.propagation_stopped) return;
  }
  event.phase = EventPhase::target;
  target->invoke(event, false);
  if (event.propagation_stopped) return;
  event.phase = EventPhase::bubble;
  for (auto iterator = path.rbegin(); iterator != path.rend(); ++iterator) {
    if (*iterator == target) continue;
    (*iterator)->invoke(event, false);
    if (event.propagation_stopped) return;
  }
}

void Document::pointer_move(float x, float y) {
  auto* next = hit_test(x, y);
  if (next == hovered_) return;
  if (hovered_) hovered_->set_hovered(false);
  hovered_ = next;
  if (hovered_) hovered_->set_hovered(true);
  if (style_sheet_) style_sheet_->apply(*root_);
}

void Document::scroll(float x, float y, float delta_y) {
  auto* target = hit_test(x, y);
  while (target && target->style().overflow != Overflow::scroll &&
         target->style().overflow != Overflow::automatic)
    target = target->parent();
  if (!target) return;
  target->scroll_by(delta_y);
  if (viewport_width_ > 0 && viewport_height_ > 0)
    layout(viewport_width_, viewport_height_);
}

void Document::focus(Node* node) {
  if (node && (!node->focusable() || node->disabled())) node = nullptr;
  if (node == focused_) return;
  if (focused_) {
    focused_->set_focused(false);
    UiEvent blur;
    blur.type = "blur";
    focused_->invoke(blur, false);
  }
  focused_ = node;
  if (focused_) {
    focused_->set_focused(true);
    UiEvent focus_event;
    focus_event.type = "focus";
    focused_->invoke(focus_event, false);
  }
  if (style_sheet_) style_sheet_->apply(*root_);
}

void Document::focus_next(bool reverse) {
  std::vector<Node*> nodes;
  focusable_nodes(*root_, nodes);
  std::stable_sort(nodes.begin(), nodes.end(), [](const Node* left,
                                                   const Node* right) {
    const auto left_index = left->tab_index() < 0 ? 0 : left->tab_index();
    const auto right_index = right->tab_index() < 0 ? 0 : right->tab_index();
    return left_index < right_index;
  });
  if (nodes.empty()) {
    focus(nullptr);
    return;
  }
  const auto found = std::ranges::find(nodes, focused_);
  if (found == nodes.end()) {
    focus(reverse ? nodes.back() : nodes.front());
    return;
  }
  auto index = static_cast<std::size_t>(std::distance(nodes.begin(), found));
  index = reverse ? (index == 0 ? nodes.size() - 1 : index - 1)
                  : (index + 1) % nodes.size();
  focus(nodes[index]);
}

void Document::text_input(std::string_view utf8) {
  if (!focused_ || !focused_->editable() || focused_->disabled()) return;
  focused_->set_text(focused_->text() + std::string(utf8));
  UiEvent input;
  input.type = "input";
  input.text = std::string(utf8);
  focused_->invoke(input, false);
  if (viewport_width_ > 0 && viewport_height_ > 0)
    layout(viewport_width_, viewport_height_);
}

void Document::key_input(std::uint32_t key, bool shift) {
  if (key == 9U) {
    focus_next(shift);
    return;
  }
  if (!focused_ || !focused_->editable() || focused_->disabled()) return;
  if (key == 8U && !focused_->text().empty()) {
    auto value = focused_->text();
    auto position = value.size() - 1;
    while (position > 0 &&
           (static_cast<unsigned char>(value[position]) & 0xc0U) == 0x80U)
      --position;
    value.erase(position);
    focused_->set_text(std::move(value));
  }
  UiEvent event;
  event.type = "keydown";
  event.key = key;
  focused_->invoke(event, false);
  if (viewport_width_ > 0 && viewport_height_ > 0)
    layout(viewport_width_, viewport_height_);
}

tokmon::Json Document::accessibility_tree() const {
  return accessible(*root_);
}

Node* Document::find_by_id(std::string_view id) {
  return find_id(*root_, id);
}

} // namespace white
