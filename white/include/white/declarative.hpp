#pragma once

#include <white/document.hpp>
#include <white/renderer.hpp>

#include <tokmon/common/types.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace white {

struct EventBinding {
  std::string command;
  tokmon::Json arguments{tokmon::Json::object()};
  bool capture{false};
};

// The immutable syntax tree used by White's QML-like JSON UI documents.
// Values may contain {"$bind":"path.to.value"} expressions.
struct Element {
  std::string type{"Item"};
  std::string id;
  std::string key;
  tokmon::Json properties{tokmon::Json::object()};
  std::map<std::string, EventBinding, std::less<>> events;
  std::vector<Element> children;

  static Element from_json(const tokmon::Json &value,
                           std::string_view location = "root");
  [[nodiscard]] tokmon::Json to_json() const;

  Element &named(std::string value);
  Element &keyed(std::string value);
  Element &property(std::string name, tokmon::Json value);
  Element &on(std::string event, std::string command,
              tokmon::Json arguments = tokmon::Json::object(),
              bool capture = false);
  Element &append(Element child);
};

struct ViewBlueprint {
  std::string schema{"org.tokmon.white.view/v1"};
  std::vector<std::string> imports;
  std::string style_sheet;
  Element root;
  std::map<std::string, Element, std::less<>> components;

  static ViewBlueprint parse(std::string_view json_document);
  static ViewBlueprint load(const std::filesystem::path &json_file);
  static ViewBlueprint from_json(const tokmon::Json &value);
  [[nodiscard]] tokmon::Json to_json() const;
};

struct ComponentDefinition {
  std::string tag{"div"};
  tokmon::Json defaults{tokmon::Json::object()};
  std::string accessible_role;
};

// Registries are explicit and copyable, so an application can derive its own
// component vocabulary without modifying White or relying on global state.
class ComponentRegistry final {
public:
  ComponentRegistry();

  void register_native(std::string name, ComponentDefinition definition);
  void register_component(std::string name, Element component);
  [[nodiscard]] bool contains(std::string_view name) const;
  [[nodiscard]] const ComponentDefinition *
  native(std::string_view name) const noexcept;
  [[nodiscard]] const Element *component(std::string_view name) const noexcept;

private:
  std::map<std::string, ComponentDefinition, std::less<>> native_;
  std::map<std::string, Element, std::less<>> components_;
};

// UI state is a JSON value on purpose: state snapshots cross the Snow/White
// boundary without binding product objects into the rendering toolkit.
class ViewState final {
public:
  explicit ViewState(tokmon::Json value = tokmon::Json::object());

  [[nodiscard]] const tokmon::Json &snapshot() const noexcept { return value_; }
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] tokmon::Json value(std::string_view path,
                                   tokmon::Json fallback = {}) const;
  void replace(tokmon::Json value);
  void set(std::string_view path, tokmon::Json value);
  void transact(const std::function<void(tokmon::Json &)> &update);

private:
  tokmon::Json value_;
  std::uint64_t revision_{0};
};

struct Command {
  std::string name;
  tokmon::Json arguments{tokmon::Json::object()};
  std::string source_id;
  std::string source_key;
  UiEvent event;
};

// DeclarativeView owns parsing, component expansion, data binding, keyed
// presentation-state reconciliation, layout, event-to-command routing and
// rendering. The blueprint is parsed once; only the resolved tree is refreshed.
class DeclarativeView final {
public:
  using CommandHandler = std::function<void(const Command &)>;

  explicit DeclarativeView(ViewBlueprint blueprint,
                           ComponentRegistry registry = ComponentRegistry{});
  static std::unique_ptr<DeclarativeView>
  parse(std::string_view json_document,
        ComponentRegistry registry = ComponentRegistry{});
  static std::unique_ptr<DeclarativeView>
  load(const std::filesystem::path &json_file,
       ComponentRegistry registry = ComponentRegistry{});

  [[nodiscard]] const ViewBlueprint &blueprint() const noexcept {
    return blueprint_;
  }
  [[nodiscard]] ViewState &state() noexcept { return state_; }
  [[nodiscard]] const ViewState &state() const noexcept { return state_; }
  void set_state(tokmon::Json snapshot);
  void set_command_handler(CommandHandler handler);
  void update();

  void layout(float width, float height);
  void render(RasterSurface &surface);
  void dispatch(UiEvent event);
  void pointer_move(float x, float y);
  void scroll(float x, float y, float delta_y);
  void text_input(std::string_view utf8);
  void key_input(std::uint32_t key, bool shift = false);

  [[nodiscard]] Document &document() noexcept { return document_; }
  [[nodiscard]] const Document &document() const noexcept { return document_; }
  [[nodiscard]] Node *find_by_id(std::string_view id);

private:
  ViewBlueprint blueprint_;
  ComponentRegistry registry_;
  ViewState state_;
  Document document_;
  CommandHandler command_handler_;
  std::uint64_t rendered_revision_{static_cast<std::uint64_t>(-1)};
  float viewport_width_{0};
  float viewport_height_{0};
  std::map<std::string, float, std::less<>> pending_scroll_;
  std::string pending_focus_;
};

} // namespace white
