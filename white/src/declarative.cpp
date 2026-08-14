#include <white/declarative.hpp>

#include <tokmon/common/files.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace white {
namespace {

constexpr std::string_view view_schema = "org.tokmon.white.view/v1";

[[noreturn]] void invalid(std::string_view location, std::string message) {
  throw tokmon::Error("white.view.document",
                      std::string(location) + ": " + std::move(message));
}

bool truthy(const tokmon::Json &value) {
  if (value.is_null())
    return false;
  if (value.is_boolean())
    return value.get<bool>();
  if (value.is_number())
    return value.get<double>() != 0.0;
  if (value.is_string())
    return !value.get_ref<const std::string &>().empty();
  return !value.empty();
}

std::vector<std::string> split_path(std::string_view path) {
  std::vector<std::string> result;
  std::stringstream input{std::string(path)};
  std::string item;
  while (std::getline(input, item, '.')) {
    if (!item.empty())
      result.push_back(std::move(item));
  }
  return result;
}

const tokmon::Json *descend(const tokmon::Json &root, std::string_view path) {
  const auto parts = split_path(path);
  const tokmon::Json *current = &root;
  for (const auto &part : parts) {
    if (current->is_object()) {
      const auto found = current->find(part);
      if (found == current->end())
        return nullptr;
      current = &*found;
      continue;
    }
    if (current->is_array()) {
      try {
        const auto index = static_cast<std::size_t>(std::stoull(part));
        if (index >= current->size())
          return nullptr;
        current = &(*current)[index];
        continue;
      } catch (...) {
        return nullptr;
      }
    }
    return nullptr;
  }
  return current;
}

const tokmon::Json *lookup(const tokmon::Json &state, const tokmon::Json &scope,
                           std::string_view path) {
  const auto parts = split_path(path);
  if (parts.empty())
    return &state;
  if (scope.is_object()) {
    const auto local = scope.find(parts.front());
    if (local != scope.end()) {
      if (parts.size() == 1)
        return &*local;
      std::string remainder;
      for (std::size_t index = 1; index < parts.size(); ++index) {
        if (!remainder.empty())
          remainder += '.';
        remainder += parts[index];
      }
      return descend(*local, remainder);
    }
  }
  return descend(state, path);
}

tokmon::Json resolve_value(const tokmon::Json &value, const tokmon::Json &state,
                           const tokmon::Json &scope) {
  if (value.is_array()) {
    auto result = tokmon::Json::array();
    for (const auto &item : value)
      result.push_back(resolve_value(item, state, scope));
    return result;
  }
  if (!value.is_object())
    return value;
  if (const auto binding = value.find("$bind"); binding != value.end()) {
    if (!binding->is_string())
      invalid("binding", "$bind must contain a dotted string path");
    const auto *found = lookup(state, scope, binding->get<std::string>());
    if (found)
      return *found;
    const auto fallback = value.find("default");
    return fallback == value.end() ? tokmon::Json{}
                                   : resolve_value(*fallback, state, scope);
  }
  if (const auto expression = value.find("$not"); expression != value.end())
    return !truthy(resolve_value(*expression, state, scope));
  if (const auto expression = value.find("$eq"); expression != value.end()) {
    if (!expression->is_array() || expression->size() != 2)
      invalid("binding", "$eq requires exactly two operands");
    return resolve_value((*expression)[0], state, scope) ==
           resolve_value((*expression)[1], state, scope);
  }
  if (const auto expression = value.find("$and"); expression != value.end()) {
    if (!expression->is_array())
      invalid("binding", "$and requires an array");
    return std::ranges::all_of(*expression, [&](const auto &item) {
      return truthy(resolve_value(item, state, scope));
    });
  }
  if (const auto expression = value.find("$or"); expression != value.end()) {
    if (!expression->is_array())
      invalid("binding", "$or requires an array");
    return std::ranges::any_of(*expression, [&](const auto &item) {
      return truthy(resolve_value(item, state, scope));
    });
  }
  if (const auto expression = value.find("$concat");
      expression != value.end()) {
    if (!expression->is_array())
      invalid("binding", "$concat requires an array");
    std::string result;
    for (const auto &item : *expression) {
      const auto resolved = resolve_value(item, state, scope);
      result +=
          resolved.is_string() ? resolved.get<std::string>() : resolved.dump();
    }
    return result;
  }
  auto result = tokmon::Json::object();
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator)
    result[iterator.key()] = resolve_value(iterator.value(), state, scope);
  return result;
}

std::string scalar_text(const tokmon::Json &value) {
  if (value.is_null())
    return {};
  if (value.is_string())
    return value.get<std::string>();
  if (value.is_boolean())
    return value.get<bool>() ? "true" : "false";
  if (value.is_number())
    return value.dump();
  return value.dump();
}

std::string kebab(std::string value) {
  std::string result;
  for (const auto character : value) {
    if (std::isupper(static_cast<unsigned char>(character))) {
      if (!result.empty())
        result.push_back('-');
      result.push_back(static_cast<char>(
          std::tolower(static_cast<unsigned char>(character))));
    } else {
      result.push_back(character);
    }
  }
  return result;
}

std::string css_scalar(const tokmon::Json &value) {
  if (value.is_string())
    return value.get<std::string>();
  if (value.is_number_float())
    return std::to_string(value.get<double>());
  if (value.is_number_integer())
    return std::to_string(value.get<long long>());
  if (value.is_number_unsigned())
    return std::to_string(value.get<unsigned long long>());
  if (value.is_boolean())
    return value.get<bool>() ? "true" : "false";
  return {};
}

bool style_property(std::string_view name) {
  static constexpr std::string_view names[] = {
      "width",          "height",
      "minWidth",       "minHeight",
      "maxWidth",       "maxHeight",
      "left",           "top",
      "right",          "bottom",
      "position",       "flexGrow",
      "flexShrink",     "flexDirection",
      "alignItems",     "alignSelf",
      "justifyContent", "gap",
      "padding",        "paddingLeft",
      "paddingTop",     "paddingRight",
      "paddingBottom",  "margin",
      "marginLeft",     "marginTop",
      "marginRight",    "marginBottom",
      "borderWidth",    "borderRadius",
      "fontSize",       "lineHeight",
      "fontWeight",     "opacity",
      "direction",      "color",
      "background",     "backgroundColor",
      "borderColor",    "overflow"};
  return std::ranges::find(names, name) != std::end(names);
}

tokmon::Json merge_properties(tokmon::Json base, const tokmon::Json &value) {
  if (!base.is_object())
    base = tokmon::Json::object();
  if (!value.is_object())
    return base;
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
    if (iterator.key() == "style" && iterator.value().is_object() &&
        base.contains("style") && base["style"].is_object()) {
      for (auto style = iterator.value().begin();
           style != iterator.value().end(); ++style)
        base["style"][style.key()] = style.value();
    } else {
      base[iterator.key()] = iterator.value();
    }
  }
  return base;
}

std::string node_identity(const Node &node) {
  if (const auto key = node.attribute("data-white-key"); !key.empty())
    return "key:" + key;
  if (!node.id().empty())
    return "id:" + node.id();
  return {};
}

void collect_runtime_state(const Node &node,
                           std::map<std::string, float, std::less<>> &scroll,
                           std::string &focused) {
  const auto identity = node_identity(node);
  if (!identity.empty()) {
    if (node.scroll_offset_y() > 0)
      scroll[identity] = node.scroll_offset_y();
    if (node.focused())
      focused = identity;
  }
  for (const auto &child : node.children())
    collect_runtime_state(*child, scroll, focused);
}

Node *find_identity(Node &node, std::string_view identity) {
  if (node_identity(node) == identity)
    return &node;
  for (auto &child : node.children()) {
    if (auto *result = find_identity(*child, identity))
      return result;
  }
  return nullptr;
}

void restore_scroll(Node &node,
                    const std::map<std::string, float, std::less<>> &scroll) {
  const auto identity = node_identity(node);
  if (const auto found = scroll.find(identity); found != scroll.end())
    node.set_scroll_offset(found->second);
  for (auto &child : node.children())
    restore_scroll(*child, scroll);
}

struct SlotContent {
  const std::vector<Element> *children{nullptr};
  tokmon::Json scope{tokmon::Json::object()};
};

class Resolver final {
public:
  Resolver(const tokmon::Json &state, const ComponentRegistry &registry,
           const DeclarativeView::CommandHandler &command_handler)
      : state_(state), registry_(registry), command_handler_(command_handler) {}

  std::vector<std::unique_ptr<Node>>
  resolve(const Element &element, const tokmon::Json &scope,
          const SlotContent *slot = nullptr) const {
    if (element.type == "If")
      return resolve_if(element, scope, slot);
    if (element.type == "Repeater")
      return resolve_repeater(element, scope, slot);
    if (element.type == "Slot")
      return resolve_slot(slot);
    if (const auto *component = registry_.component(element.type)) {
      auto props = resolve_value(element.properties, state_, scope);
      auto component_scope = scope;
      component_scope["props"] = std::move(props);
      auto expanded = *component;
      if (!element.id.empty())
        expanded.id = element.id;
      if (!element.key.empty())
        expanded.key = element.key;
      for (const auto &[name, binding] : element.events)
        expanded.events[name] = binding;
      const SlotContent invocation{&element.children, scope};
      return resolve(expanded, component_scope, &invocation);
    }
    return resolve_native(element, scope, slot);
  }

private:
  std::vector<std::unique_ptr<Node>> resolve_if(const Element &element,
                                                const tokmon::Json &scope,
                                                const SlotContent *slot) const {
    const auto condition =
        element.properties.contains("condition")
            ? element.properties["condition"]
            : element.properties.value("when", tokmon::Json{});
    if (!truthy(resolve_value(condition, state_, scope)))
      return {};
    std::vector<std::unique_ptr<Node>> result;
    for (const auto &child : element.children) {
      auto expanded = resolve(child, scope, slot);
      std::ranges::move(expanded, std::back_inserter(result));
    }
    return result;
  }

  std::vector<std::unique_ptr<Node>>
  resolve_repeater(const Element &element, const tokmon::Json &scope,
                   const SlotContent *slot) const {
    const auto model =
        resolve_value(element.properties.value("model", tokmon::Json::array()),
                      state_, scope);
    const auto item_name = element.properties.value("as", "item");
    const auto index_name = element.properties.value("indexAs", "index");
    const auto key_path = element.properties.value("keyPath", "");
    std::vector<std::unique_ptr<Node>> result;
    const auto append_item = [&](const tokmon::Json &item,
                                 const tokmon::Json &index,
                                 std::string fallback_key) {
      auto item_scope = scope;
      item_scope[item_name] = item;
      item_scope[index_name] = index;
      std::string item_key = std::move(fallback_key);
      if (!key_path.empty()) {
        if (const auto *value = descend(item, key_path))
          item_key = scalar_text(*value);
      }
      for (const auto &child : element.children) {
        auto expanded = resolve(child, item_scope, slot);
        for (std::size_t child_index = 0; child_index < expanded.size();
             ++child_index) {
          if (expanded[child_index]->attribute("data-white-key").empty()) {
            expanded[child_index]->set_attribute(
                "data-white-key", element.key + ":" + item_key + ":" +
                                      std::to_string(child_index));
          }
          result.push_back(std::move(expanded[child_index]));
        }
      }
    };
    if (model.is_array()) {
      for (std::size_t index = 0; index < model.size(); ++index)
        append_item(model[index], index, std::to_string(index));
    } else if (model.is_object()) {
      std::size_t index = 0;
      for (auto iterator = model.begin(); iterator != model.end(); ++iterator)
        append_item(iterator.value(), index++, iterator.key());
    }
    return result;
  }

  std::vector<std::unique_ptr<Node>>
  resolve_slot(const SlotContent *slot) const {
    std::vector<std::unique_ptr<Node>> result;
    if (!slot || !slot->children)
      return result;
    for (const auto &child : *slot->children) {
      auto expanded = resolve(child, slot->scope, nullptr);
      std::ranges::move(expanded, std::back_inserter(result));
    }
    return result;
  }

  std::vector<std::unique_ptr<Node>>
  resolve_native(const Element &element, const tokmon::Json &scope,
                 const SlotContent *slot) const {
    const auto *definition = registry_.native(element.type);
    if (!definition)
      invalid(element.id.empty() ? element.type : element.id,
              "unknown component type '" + element.type + "'");
    auto properties = merge_properties(
        definition->defaults, resolve_value(element.properties, state_, scope));
    if (properties.contains("visible") && !truthy(properties["visible"]))
      return {};

    auto node = std::make_unique<Node>(definition->tag);
    if (!element.id.empty())
      node->set_id(element.id);
    auto key = element.key;
    if (properties.contains("key"))
      key = scalar_text(properties["key"]);
    if (!key.empty())
      node->set_attribute("data-white-key", key);
    if (!definition->accessible_role.empty())
      node->set_attribute("role", definition->accessible_role);

    if (const auto text = properties.find("text"); text != properties.end())
      node->set_text(scalar_text(*text));
    if (const auto value = properties.find("value");
        value != properties.end()) {
      node->set_text(scalar_text(*value));
      node->set_attribute("value", scalar_text(*value));
    }
    if (const auto classes = properties.find("class");
        classes != properties.end())
      node->set_attribute("class", scalar_text(*classes));
    if (const auto classes = properties.find("classes");
        classes != properties.end() && classes->is_array()) {
      for (const auto &value : *classes)
        node->add_class(scalar_text(value));
    }
    if ((properties.contains("disabled") && truthy(properties["disabled"])) ||
        (properties.contains("enabled") && !truthy(properties["enabled"])))
      node->set_attribute("disabled", "true");
    if (properties.contains("editable") && truthy(properties["editable"]))
      node->set_attribute("contenteditable", "plaintext-only");
    if (properties.contains("tabIndex"))
      node->set_attribute("tabindex", scalar_text(properties["tabIndex"]));
    if (properties.contains("role"))
      node->set_attribute("role", scalar_text(properties["role"]));
    if (properties.contains("ariaLabel"))
      node->set_attribute("aria-label", scalar_text(properties["ariaLabel"]));
    if (properties.contains("attributes") &&
        properties["attributes"].is_object()) {
      for (auto iterator = properties["attributes"].begin();
           iterator != properties["attributes"].end(); ++iterator)
        node->set_attribute(iterator.key(), scalar_text(iterator.value()));
    }

    std::string inline_style;
    const auto append_style = [&](std::string name, const tokmon::Json &value) {
      const auto converted = css_scalar(value);
      if (converted.empty())
        return;
      inline_style += kebab(std::move(name)) + ":" + converted + ";";
    };
    if (properties.contains("style")) {
      if (properties["style"].is_string()) {
        inline_style += properties["style"].get<std::string>();
        if (!inline_style.ends_with(';'))
          inline_style += ';';
      } else if (properties["style"].is_object()) {
        for (auto iterator = properties["style"].begin();
             iterator != properties["style"].end(); ++iterator)
          append_style(iterator.key(), iterator.value());
      }
    }
    for (auto iterator = properties.begin(); iterator != properties.end();
         ++iterator) {
      if (style_property(iterator.key()))
        append_style(iterator.key(), iterator.value());
    }
    if (!inline_style.empty())
      node->set_attribute("style", inline_style);

    for (const auto &[event_name, binding] : element.events) {
      auto command = Command{binding.command,
                             resolve_value(binding.arguments, state_, scope),
                             node->id(),
                             key,
                             {}};
      const auto *source = node.get();
      node->on(
          event_name,
          [handler = command_handler_, command = std::move(command),
           source](UiEvent &event) mutable {
            if (!handler)
              return;
            command.event = event;
            command.source_key = source->attribute("data-white-key");
            if (!command.arguments.is_object())
              command.arguments = {{"payload", command.arguments}};
            command.arguments["value"] = source->text();
            handler(command);
          },
          binding.capture);
    }
    for (const auto &child : element.children) {
      auto expanded = resolve(child, scope, slot);
      for (auto &item : expanded)
        node->append(std::move(item));
    }
    std::vector<std::unique_ptr<Node>> result;
    result.push_back(std::move(node));
    return result;
  }

  const tokmon::Json &state_;
  const ComponentRegistry &registry_;
  const DeclarativeView::CommandHandler &command_handler_;
};

constexpr std::string_view default_styles = R"css(
  body { color: #202124; background: transparent; }
  button { height: 34px; padding: 8px; border-radius: 8px;
           background: #f4f5f7; color: #202124; }
  button:hover { background: #e9ebef; }
  button:focus, input:focus, textarea:focus { border-width: 1px;
           border-color: #5b7cfa; }
  input, textarea { min-height: 34px; padding: 8px; border-width: 1px;
           border-color: #d8dbe2; border-radius: 8px; background: white; }
)css";

} // namespace

Element Element::from_json(const tokmon::Json &value,
                           std::string_view location) {
  if (!value.is_object())
    invalid(location, "element must be an object");
  Element result;
  if (!value.contains("type") || !value["type"].is_string())
    invalid(location, "element requires a string 'type'");
  result.type = value["type"].get<std::string>();
  if (value.contains("id"))
    result.id = value["id"].get<std::string>();
  if (value.contains("key"))
    result.key = value["key"].get<std::string>();
  if (value.contains("properties")) {
    if (!value["properties"].is_object())
      invalid(location, "properties must be an object");
    result.properties = value["properties"];
  }
  static const std::vector<std::string> reserved = {
      "type", "id", "key", "properties", "on", "children"};
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
    if (std::ranges::find(reserved, iterator.key()) == reserved.end())
      result.properties[iterator.key()] = iterator.value();
  }
  if (value.contains("on")) {
    if (!value["on"].is_object())
      invalid(location, "on must be an object");
    for (auto iterator = value["on"].begin(); iterator != value["on"].end();
         ++iterator) {
      EventBinding binding;
      if (iterator.value().is_string()) {
        binding.command = iterator.value().get<std::string>();
      } else if (iterator.value().is_object()) {
        binding.command = iterator.value().value("command", "");
        binding.arguments = iterator.value().value(
            "arguments",
            iterator.value().value("args", tokmon::Json::object()));
        binding.capture = iterator.value().value("capture", false);
      } else {
        invalid(location, "event binding must be a command string or object");
      }
      if (binding.command.empty())
        invalid(location, "event command is empty");
      result.events[iterator.key()] = std::move(binding);
    }
  }
  if (value.contains("children")) {
    if (!value["children"].is_array())
      invalid(location, "children must be an array");
    std::size_t index = 0;
    for (const auto &child : value["children"])
      result.children.push_back(
          from_json(child, std::string(location) + ".children[" +
                               std::to_string(index++) + "]"));
  }
  return result;
}

tokmon::Json Element::to_json() const {
  tokmon::Json result{{"type", type}};
  if (!id.empty())
    result["id"] = id;
  if (!key.empty())
    result["key"] = key;
  if (!properties.empty())
    result["properties"] = properties;
  if (!events.empty()) {
    result["on"] = tokmon::Json::object();
    for (const auto &[name, binding] : events) {
      result["on"][name] = {{"command", binding.command},
                            {"arguments", binding.arguments},
                            {"capture", binding.capture}};
    }
  }
  if (!children.empty()) {
    result["children"] = tokmon::Json::array();
    for (const auto &child : children)
      result["children"].push_back(child.to_json());
  }
  return result;
}

Element &Element::named(std::string value) {
  id = std::move(value);
  return *this;
}

Element &Element::keyed(std::string value) {
  key = std::move(value);
  return *this;
}

Element &Element::property(std::string name, tokmon::Json value) {
  properties[std::move(name)] = std::move(value);
  return *this;
}

Element &Element::on(std::string event, std::string command,
                     tokmon::Json arguments, bool capture) {
  events[std::move(event)] = {std::move(command), std::move(arguments),
                              capture};
  return *this;
}

Element &Element::append(Element child) {
  children.push_back(std::move(child));
  return *this;
}

ViewBlueprint ViewBlueprint::parse(std::string_view json_document) {
  try {
    return from_json(tokmon::Json::parse(json_document));
  } catch (const tokmon::Error &) {
    throw;
  } catch (const std::exception &error) {
    throw tokmon::Error("white.view.json", error.what());
  }
}

ViewBlueprint ViewBlueprint::load(const std::filesystem::path &json_file) {
  if (json_file.extension() != ".json")
    throw tokmon::Error("white.view.extension",
                        "White UI documents must use the .json extension");
  return parse(tokmon::read_text_file(json_file));
}

ViewBlueprint ViewBlueprint::from_json(const tokmon::Json &value) {
  if (!value.is_object())
    invalid("document", "root must be an object");
  ViewBlueprint result;
  result.schema = value.value("schema", std::string(view_schema));
  if (result.schema != view_schema)
    invalid("document.schema", "unsupported schema '" + result.schema + "'");
  if (value.contains("imports")) {
    if (!value["imports"].is_array())
      invalid("document.imports", "imports must be an array");
    for (const auto &item : value["imports"])
      result.imports.push_back(item.get<std::string>());
  }
  result.style_sheet = value.value("styles", "");
  if (!value.contains("root"))
    invalid("document", "missing root element");
  result.root = Element::from_json(value["root"]);
  if (value.contains("components")) {
    if (!value["components"].is_object())
      invalid("document.components", "components must be an object");
    for (auto iterator = value["components"].begin();
         iterator != value["components"].end(); ++iterator)
      result.components[iterator.key()] =
          Element::from_json(iterator.value(), "components." + iterator.key());
  }
  return result;
}

tokmon::Json ViewBlueprint::to_json() const {
  tokmon::Json result{{"schema", schema},
                      {"imports", imports},
                      {"styles", style_sheet},
                      {"root", root.to_json()}};
  if (!components.empty()) {
    result["components"] = tokmon::Json::object();
    for (const auto &[name, component] : components)
      result["components"][name] = component.to_json();
  }
  return result;
}

ComponentRegistry::ComponentRegistry() {
  register_native("Application", {"body", {{"flexGrow", 1}}, "application"});
  register_native("Item", {"div", tokmon::Json::object(), "group"});
  register_native("Row", {"div", {{"flexDirection", "row"}}, "group"});
  register_native("Column", {"div", {{"flexDirection", "column"}}, "group"});
  register_native("Text", {"span", tokmon::Json::object(), "text"});
  register_native("Button", {"button", tokmon::Json::object(), "button"});
  register_native("TextField", {"input", tokmon::Json::object(), "textbox"});
  register_native("TextArea", {"textarea", tokmon::Json::object(), "textbox"});
  register_native("ScrollView", {"div", {{"overflow", "auto"}}, "scrollarea"});
  register_native("SplitPane", {"div", {{"flexDirection", "row"}}, "group"});
  register_native("Dialog", {"div", {{"class", "white-dialog"}}, "dialog"});
  register_native("Spacer", {"div", {{"flexGrow", 1}}, "none"});
  register_native("Badge", {"span", {{"class", "white-badge"}}, "status"});
  register_native("IconButton",
                  {"button", {{"class", "white-icon-button"}}, "button"});
  register_native("ListView", {"div", {{"overflow", "auto"}}, "list"});
  register_native("ListItem", {"div", tokmon::Json::object(), "listitem"});
  register_native("Menu", {"div", {{"class", "white-menu"}}, "menu"});
  register_native("MenuItem", {"button", tokmon::Json::object(), "menuitem"});
  register_native("Toolbar", {"div", {{"flexDirection", "row"}}, "toolbar"});
  register_native("Tabs", {"div", {{"flexDirection", "row"}}, "tablist"});
  register_native("Tab", {"button", tokmon::Json::object(), "tab"});
  register_native("CheckBox", {"input", tokmon::Json::object(), "checkbox"});
  register_native(
      "Separator",
      {"div", {{"height", 1}, {"background", "#e4e6eb"}}, "separator"});
  register_native(
      "ProgressBar",
      {"div", {{"height", 4}, {"background", "#e4e6eb"}}, "progressbar"});
  register_native("Overlay", {"div",
                              {{"position", "absolute"},
                               {"left", 0},
                               {"top", 0},
                               {"right", 0},
                               {"bottom", 0}},
                              "presentation"});
}

void ComponentRegistry::register_native(std::string name,
                                        ComponentDefinition definition) {
  if (name.empty())
    throw tokmon::Error("white.component.name", "component name is empty");
  native_[std::move(name)] = std::move(definition);
}

void ComponentRegistry::register_component(std::string name,
                                           Element component) {
  if (name.empty())
    throw tokmon::Error("white.component.name", "component name is empty");
  components_[std::move(name)] = std::move(component);
}

bool ComponentRegistry::contains(std::string_view name) const {
  return native_.contains(name) || components_.contains(name);
}

const ComponentDefinition *
ComponentRegistry::native(std::string_view name) const noexcept {
  const auto found = native_.find(name);
  return found == native_.end() ? nullptr : &found->second;
}

const Element *
ComponentRegistry::component(std::string_view name) const noexcept {
  const auto found = components_.find(name);
  return found == components_.end() ? nullptr : &found->second;
}

ViewState::ViewState(tokmon::Json value) : value_(std::move(value)) {
  if (!value_.is_object())
    throw tokmon::Error("white.state.root", "view state must be an object");
}

tokmon::Json ViewState::value(std::string_view path,
                              tokmon::Json fallback) const {
  const auto *found = descend(value_, path);
  return found ? *found : std::move(fallback);
}

void ViewState::replace(tokmon::Json value) {
  if (!value.is_object())
    throw tokmon::Error("white.state.root", "view state must be an object");
  if (value_ == value)
    return;
  value_ = std::move(value);
  ++revision_;
}

void ViewState::set(std::string_view path, tokmon::Json value) {
  const auto parts = split_path(path);
  if (parts.empty()) {
    replace(std::move(value));
    return;
  }
  auto *current = &value_;
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    if (!current->is_object())
      *current = tokmon::Json::object();
    current = &(*current)[parts[index]];
  }
  if (!current->is_object())
    *current = tokmon::Json::object();
  (*current)[parts.back()] = std::move(value);
  ++revision_;
}

void ViewState::transact(const std::function<void(tokmon::Json &)> &update) {
  auto next = value_;
  update(next);
  replace(std::move(next));
}

DeclarativeView::DeclarativeView(ViewBlueprint blueprint,
                                 ComponentRegistry registry)
    : blueprint_(std::move(blueprint)), registry_(std::move(registry)) {
  for (const auto &[name, component] : blueprint_.components)
    registry_.register_component(name, component);
}

std::unique_ptr<DeclarativeView>
DeclarativeView::parse(std::string_view json_document,
                       ComponentRegistry registry) {
  return std::make_unique<DeclarativeView>(ViewBlueprint::parse(json_document),
                                           std::move(registry));
}

std::unique_ptr<DeclarativeView>
DeclarativeView::load(const std::filesystem::path &json_file,
                      ComponentRegistry registry) {
  return std::make_unique<DeclarativeView>(ViewBlueprint::load(json_file),
                                           std::move(registry));
}

void DeclarativeView::set_state(tokmon::Json snapshot) {
  state_.replace(std::move(snapshot));
}

void DeclarativeView::set_command_handler(CommandHandler handler) {
  command_handler_ = std::move(handler);
  rendered_revision_ = static_cast<std::uint64_t>(-1);
}

void DeclarativeView::update() {
  if (rendered_revision_ == state_.revision())
    return;
  collect_runtime_state(document_.root(), pending_scroll_, pending_focus_);
  Resolver resolver(state_.snapshot(), registry_, command_handler_);
  const auto scope = tokmon::Json::object();
  auto roots = resolver.resolve(blueprint_.root, scope);
  if (roots.size() != 1)
    throw tokmon::Error("white.view.root",
                        "root must resolve to exactly one visual element");
  document_.set_root(std::move(roots.front()));
  document_.set_style_sheet(
      StyleSheet::parse(std::string(default_styles) + blueprint_.style_sheet));
  rendered_revision_ = state_.revision();
  if (viewport_width_ > 0 && viewport_height_ > 0) {
    document_.layout(viewport_width_, viewport_height_);
    restore_scroll(document_.root(), pending_scroll_);
    document_.layout(viewport_width_, viewport_height_);
  }
  if (!pending_focus_.empty())
    document_.focus(find_identity(document_.root(), pending_focus_));
  pending_scroll_.clear();
  pending_focus_.clear();
}

void DeclarativeView::layout(float width, float height) {
  viewport_width_ = width;
  viewport_height_ = height;
  update();
  document_.layout(width, height);
  if (!pending_scroll_.empty()) {
    restore_scroll(document_.root(), pending_scroll_);
    document_.layout(width, height);
    pending_scroll_.clear();
  }
}

void DeclarativeView::render(RasterSurface &surface) {
  update();
  surface.render(document_);
}

void DeclarativeView::dispatch(UiEvent event) {
  update();
  document_.dispatch(std::move(event));
}

void DeclarativeView::pointer_move(float x, float y) {
  update();
  document_.pointer_move(x, y);
}

void DeclarativeView::scroll(float x, float y, float delta_y) {
  update();
  document_.scroll(x, y, delta_y);
}

void DeclarativeView::text_input(std::string_view utf8) {
  update();
  document_.text_input(utf8);
}

void DeclarativeView::key_input(std::uint32_t key, bool shift) {
  update();
  document_.key_input(key, shift);
}

Node *DeclarativeView::find_by_id(std::string_view id) {
  update();
  return document_.find_by_id(id);
}

} // namespace white
