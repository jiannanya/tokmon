#include <tokmon/settings.hpp>

#include <snow/config.hpp>
#include <tokmon/common/files.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <set>

namespace tokmon::desktop {
namespace {

constexpr std::array<std::string_view, 6> required_plugins = {
    "session", "storage", "model", "tools", "policy", "agent"};

bool is_required(std::string_view instance) {
  return std::ranges::find(required_plugins, instance) !=
         required_plugins.end();
}

std::uint64_t positive_integer(std::string_view text, std::uint64_t fallback,
                               std::uint64_t minimum, std::uint64_t maximum) {
  std::uint64_t value{};
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    return fallback;
  return std::clamp(value, minimum, maximum);
}

tokmon::Json load_existing(const std::filesystem::path &path,
                           tokmon::Json fallback) {
  return snow::load_json_config(path, std::move(fallback));
}

void write_json(const std::filesystem::path &path, const tokmon::Json &value) {
  tokmon::write_text_file_atomic(path, value.dump(2) + "\n");
}

} // namespace

DesktopSettings load_desktop_settings(const std::filesystem::path &workspace,
                                      std::string config_dir_name) {
  DesktopSettings result;
  result.config_dir_name = std::move(config_dir_name);
  const auto layout = snow::ConfigLayout::resolve(
      std::filesystem::weakly_canonical(workspace), result.config_dir_name);

  const auto product =
      load_existing(layout.config_root / "tokmon.json", tokmon::Json::object());
  const auto snow_config = product.value("snow", tokmon::Json::object());
  const auto restart = snow_config.value("restart", tokmon::Json::object());
  const auto ui = product.value("ui", tokmon::Json::object());
  const auto agent = product.value("agent", tokmon::Json::object());
  result.language = ui.value("language", result.language);
  result.theme = ui.value("theme", result.theme);
  result.auto_scroll = ui.value("auto_scroll", result.auto_scroll);
  result.raw_trace = snow_config.value("raw_trace", result.raw_trace);
  result.restart_enabled = restart.value("enabled", result.restart_enabled);
  result.request_timeout_ms =
      std::to_string(snow_config.value("request_timeout_ms", 300000ULL));
  result.agent_preset = agent.value("preset", result.agent_preset);
  result.max_steps = std::to_string(agent.value("max_steps", 32ULL));
  result.model = product.value("model", result.model);

  const auto providers =
      load_existing(layout.providers, tokmon::Json::object());
  result.provider_id = providers.value("selected", result.provider_id);
  tokmon::Json provider = tokmon::Json::object();
  if (providers.contains(result.provider_id) &&
      providers[result.provider_id].is_object())
    provider = providers[result.provider_id];
  result.provider_name = provider.value("name", result.provider_name);
  result.provider_kind = provider.value("kind", result.provider_kind);
  result.endpoint = provider.value("endpoint", result.endpoint);
  result.api_key_env = provider.value("api_key_env", result.api_key_env);
  result.model = provider.value("model", result.model);

  const auto policy = load_existing(layout.policy, tokmon::Json::object());
  result.default_permission = policy.value("defaults", tokmon::Json::object())
                                  .value("mutating", result.default_permission);

  const auto composition =
      load_existing(layout.composition, tokmon::Json::object());
  const auto plugins = composition.value("plugins", tokmon::Json::array());
  if (plugins.is_array()) {
    for (const auto &entry : plugins) {
      if (!entry.is_object())
        continue;
      PluginSetting plugin;
      plugin.instance = entry.value("instance", "");
      plugin.package = entry.value("package", "");
      plugin.realm = entry.value("realm", "");
      plugin.required = is_required(plugin.instance);
      plugin.disabled =
          plugin.required ? false : entry.value("disabled", false);
      if (!plugin.instance.empty())
        result.plugins.push_back(std::move(plugin));
    }
  }
  if (result.plugins.empty()) {
    result.plugins = {
        {"session", "org.tokmon.snow.session.sqlite@1.0.0", "storage", false,
         true},
        {"storage", "org.tokmon.snow.storage.default@1.0.0", "storage", false,
         true},
        {"model", "org.tokmon.snow.model.configured@1.0.0", "model", false,
         true},
        {"tools", "org.tokmon.snow.tools.default@1.0.0", "tools", false, true},
        {"policy", "org.tokmon.snow.policy.default@1.0.0", "policy", false,
         true},
        {"agent", "org.tokmon.snow.loop.direct@1.0.0", "agent", false, true},
    };
  }
  return result;
}

void save_desktop_settings(const std::filesystem::path &workspace,
                           const DesktopSettings &settings) {
  const auto layout = snow::ConfigLayout::resolve(
      std::filesystem::weakly_canonical(workspace), settings.config_dir_name);
  std::filesystem::create_directories(layout.config_root);

  auto product =
      load_existing(layout.config_root / "tokmon.json", tokmon::Json::object());
  product["schema"] = "org.tokmon.desktop.config/v1";
  product["model"] = settings.model;
  auto &snow_config = product["snow"];
  if (!snow_config.is_object())
    snow_config = tokmon::Json::object();
  snow_config["raw_trace"] = settings.raw_trace;
  snow_config["request_timeout_ms"] =
      positive_integer(settings.request_timeout_ms, 300000, 1000, 3600000);
  auto &restart = snow_config["restart"];
  if (!restart.is_object())
    restart = tokmon::Json::object();
  restart["enabled"] = settings.restart_enabled;
  auto &ui = product["ui"];
  if (!ui.is_object())
    ui = tokmon::Json::object();
  ui["language"] = settings.language;
  ui["theme"] = settings.theme;
  ui["auto_scroll"] = settings.auto_scroll;
  auto &agent = product["agent"];
  if (!agent.is_object())
    agent = tokmon::Json::object();
  agent["preset"] = settings.agent_preset;
  agent["max_steps"] = positive_integer(settings.max_steps, 32, 1, 1024);
  write_json(layout.config_root / "tokmon.json", product);

  auto providers = load_existing(layout.providers, tokmon::Json::object());
  providers["schema"] = "org.tokmon.snow.providers/v1";
  const auto provider_id = settings.provider_id.empty() ? std::string("default")
                                                        : settings.provider_id;
  const tokmon::Json provider = {{"name", settings.provider_name},
                                 {"kind", settings.provider_kind},
                                 {"endpoint", settings.endpoint},
                                 {"api_key_env", settings.api_key_env},
                                 {"model", settings.model}};
  providers["selected"] = provider_id;
  providers[provider_id] = provider;
  // Snow's stable runtime ABI consumes providers.default. Keep it as the
  // selected-provider alias while retaining the named entry for UI editing.
  providers["default"] = provider;
  write_json(layout.providers, providers);

  auto policy = load_existing(layout.policy, tokmon::Json::object());
  policy["schema"] = "org.tokmon.snow.policy/v1";
  auto &defaults = policy["defaults"];
  if (!defaults.is_object())
    defaults = tokmon::Json::object();
  if (!defaults.contains("read_only"))
    defaults["read_only"] = "allow";
  defaults["mutating"] = settings.default_permission;
  write_json(layout.policy, policy);

  auto composition = load_existing(layout.composition, tokmon::Json::object());
  composition["schema"] = "org.tokmon.arche.composition/v1";
  if (!composition.contains("id"))
    composition["id"] = "org.tokmon.snow.default";
  auto plugins = tokmon::Json::array();
  for (const auto &plugin : settings.plugins) {
    tokmon::Json entry = {{"instance", plugin.instance},
                          {"package", plugin.package},
                          {"realm", plugin.realm}};
    if (!plugin.required && plugin.disabled)
      entry["disabled"] = true;
    plugins.push_back(std::move(entry));
  }
  composition["plugins"] = std::move(plugins);
  write_json(layout.composition, composition);
}

} // namespace tokmon::desktop
