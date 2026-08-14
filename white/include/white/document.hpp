#pragma once

#include <white/types.hpp>

#include <tokmon/common/types.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace white {

enum class EventPhase { capture, target, bubble };

struct UiEvent {
  std::string type;
  float x{0};
  float y{0};
  float delta_x{0};
  float delta_y{0};
  std::uint32_t key{0};
  std::string text;
  EventPhase phase{EventPhase::target};
  bool propagation_stopped{false};
  bool default_prevented{false};
};

class Node final {
public:
  using Handler = std::function<void(UiEvent&)>;

  explicit Node(std::string tag = "div");

  [[nodiscard]] const std::string& tag() const noexcept { return tag_; }
  [[nodiscard]] const std::string& id() const noexcept { return id_; }
  [[nodiscard]] const std::vector<std::string>& classes() const noexcept {
    return classes_;
  }
  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  [[nodiscard]] const Style& style() const noexcept { return style_; }
  [[nodiscard]] Style& style() noexcept { return style_; }
  [[nodiscard]] const Rect& layout() const noexcept { return layout_; }
  [[nodiscard]] Rect& layout() noexcept { return layout_; }
  [[nodiscard]] Node* parent() const noexcept { return parent_; }
  [[nodiscard]] bool hovered() const noexcept { return hovered_; }
  [[nodiscard]] bool focused() const noexcept { return focused_; }
  [[nodiscard]] bool disabled() const noexcept { return disabled_; }
  [[nodiscard]] bool focusable() const noexcept { return focusable_; }
  [[nodiscard]] bool editable() const noexcept { return editable_; }
  [[nodiscard]] int tab_index() const noexcept { return tab_index_; }
  [[nodiscard]] float scroll_offset_y() const noexcept {
    return scroll_offset_y_;
  }
  [[nodiscard]] float content_height() const noexcept { return content_height_; }
  [[nodiscard]] const std::vector<std::unique_ptr<Node>>& children() const {
    return children_;
  }
  [[nodiscard]] std::vector<std::unique_ptr<Node>>& children() {
    return children_;
  }

  Node& set_id(std::string id);
  Node& add_class(std::string value);
  Node& set_text(std::string value);
  Node& set_attribute(std::string name, std::string value);
  [[nodiscard]] std::string attribute(std::string_view name) const;
  Node& append(std::unique_ptr<Node> child);
  Node& on(std::string type, Handler handler, bool capture = false);
  void invoke(UiEvent& event, bool capture);
  void set_hovered(bool value) noexcept { hovered_ = value; }
  void set_focused(bool value) noexcept { focused_ = value; }
  void set_scroll_metrics(float content_height) noexcept;
  void scroll_by(float delta) noexcept;
  [[nodiscard]] std::string accessible_role() const;
  [[nodiscard]] std::string accessible_name() const;

private:
  std::string tag_;
  std::string id_;
  std::vector<std::string> classes_;
  std::string text_;
  Style style_;
  Rect layout_;
  Node* parent_{nullptr};
  std::map<std::string, std::string, std::less<>> attributes_;
  std::vector<std::unique_ptr<Node>> children_;
  std::multimap<std::pair<std::string, bool>, Handler> handlers_;
  bool hovered_{false};
  bool focused_{false};
  bool disabled_{false};
  bool focusable_{false};
  bool editable_{false};
  int tab_index_{-1};
  float scroll_offset_y_{0};
  float content_height_{0};
};

class StyleSheet final {
public:
  static StyleSheet parse(std::string_view css);
  void apply(Node& root) const;

private:
  struct Rule {
    std::string selector;
    std::map<std::string, std::string, std::less<>> declarations;
    int specificity{0};
    std::size_t order{0};
  };
  std::vector<Rule> rules_;
};

class Document final {
public:
  Document();
  static Document parse_html(std::string_view html,
                             std::string_view css = {});

  [[nodiscard]] Node& root() noexcept { return *root_; }
  [[nodiscard]] const Node& root() const noexcept { return *root_; }
  void set_style_sheet(StyleSheet style_sheet);
  void layout(float width, float height);
  [[nodiscard]] Node* hit_test(float x, float y);
  void dispatch(UiEvent event);
  void pointer_move(float x, float y);
  void scroll(float x, float y, float delta_y);
  void focus(Node* node);
  void focus_next(bool reverse = false);
  void text_input(std::string_view utf8);
  void key_input(std::uint32_t key, bool shift = false);
  [[nodiscard]] Node* focused() const noexcept { return focused_; }
  [[nodiscard]] tokmon::Json accessibility_tree() const;
  [[nodiscard]] Node* find_by_id(std::string_view id);

private:
  std::unique_ptr<Node> root_;
  std::optional<StyleSheet> style_sheet_;
  Node* focused_{nullptr};
  Node* hovered_{nullptr};
  float viewport_width_{0};
  float viewport_height_{0};
};

} // namespace white
