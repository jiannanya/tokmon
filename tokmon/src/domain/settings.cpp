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
  const auto security = product.value("security", tokmon::Json::object());
  const auto ws_cfg = product.value("workspace", tokmon::Json::object());
  const auto notif = product.value("notifications", tokmon::Json::object());
  const auto account = product.value("account", tokmon::Json::object());

  // 1. 通用
  result.language = ui.value("language", result.language);
  result.open_on_startup = ui.value("open_on_startup", result.open_on_startup);
  result.auto_save_interval = ui.value("auto_save_interval", result.auto_save_interval);
  result.update_channel = ui.value("update_channel", result.update_channel);

  // 2. 智能体与模型
  result.default_agent = agent.value("default_agent", result.default_agent);
  result.provider_mode = agent.value("provider_mode", result.provider_mode);
  result.reasoning_effort = agent.value("reasoning_effort", result.reasoning_effort);
  result.agent_preset = agent.value("preset", result.agent_preset);
  result.max_steps = std::to_string(agent.value("max_steps", 32ULL));
  result.model = product.value("model", result.model);

  // 3. 权限与安全
  result.file_access = security.value("file_access", result.file_access);
  result.command_approval = security.value("command_approval", result.command_approval);
  result.network_access = security.value("network_access", result.network_access);
  result.high_risk_confirm = security.value("high_risk_confirm", result.high_risk_confirm);

  // 4. 工作区
  result.default_workspace = ws_cfg.value("default_workspace", result.default_workspace);
  result.index_mode = ws_cfg.value("index_mode", result.index_mode);
  result.auto_sync = ws_cfg.value("auto_sync", result.auto_sync);
  result.git_integration = ws_cfg.value("git_integration", result.git_integration);

  // 5. 通知
  result.enable_notifications = notif.value("enable_notifications", result.enable_notifications);
  result.desktop_notifications = notif.value("desktop_notifications", result.desktop_notifications);
  result.message_alerts = notif.value("message_alerts", result.message_alerts);
  result.dnd_hours = notif.value("dnd_hours", result.dnd_hours);

  // 6. 外观
  result.theme = ui.value("theme", result.theme);
  result.accent_color = ui.value("accent_color", result.accent_color);
  result.ui_density = ui.value("ui_density", result.ui_density);
  result.font_size_percent = ui.value("font_size_percent", result.font_size_percent);
  result.auto_scroll = ui.value("auto_scroll", result.auto_scroll);

  // 7. 快捷键
  result.shortcut_preset = ui.value("shortcut_preset", result.shortcut_preset);

  // 8. 账户
  result.account_name = account.value("name", result.account_name);
  result.account_email = account.value("email", result.account_email);
  result.account_plan = account.value("plan", result.account_plan);
  result.cloud_sync = account.value("cloud_sync", result.cloud_sync);

  result.raw_trace = snow_config.value("raw_trace", result.raw_trace);
  result.restart_enabled = restart.value("enabled", result.restart_enabled);
  result.request_timeout_ms =
      std::to_string(snow_config.value("request_timeout_ms", 300000ULL));

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
  if (provider.contains("model"))
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
  ui["open_on_startup"] = settings.open_on_startup;
  ui["auto_save_interval"] = settings.auto_save_interval;
  ui["update_channel"] = settings.update_channel;
  ui["theme"] = settings.theme;
  ui["accent_color"] = settings.accent_color;
  ui["ui_density"] = settings.ui_density;
  ui["font_size_percent"] = settings.font_size_percent;
  ui["auto_scroll"] = settings.auto_scroll;
  ui["shortcut_preset"] = settings.shortcut_preset;

  auto &agent = product["agent"];
  if (!agent.is_object())
    agent = tokmon::Json::object();
  agent["default_agent"] = settings.default_agent;
  agent["provider_mode"] = settings.provider_mode;
  agent["reasoning_effort"] = settings.reasoning_effort;
  agent["preset"] = settings.agent_preset;
  agent["max_steps"] = positive_integer(settings.max_steps, 32, 1, 1024);

  auto &security = product["security"];
  if (!security.is_object())
    security = tokmon::Json::object();
  security["file_access"] = settings.file_access;
  security["command_approval"] = settings.command_approval;
  security["network_access"] = settings.network_access;
  security["high_risk_confirm"] = settings.high_risk_confirm;

  auto &ws_cfg = product["workspace"];
  if (!ws_cfg.is_object())
    ws_cfg = tokmon::Json::object();
  ws_cfg["default_workspace"] = settings.default_workspace;
  ws_cfg["index_mode"] = settings.index_mode;
  ws_cfg["auto_sync"] = settings.auto_sync;
  ws_cfg["git_integration"] = settings.git_integration;

  auto &notif = product["notifications"];
  if (!notif.is_object())
    notif = tokmon::Json::object();
  notif["enable_notifications"] = settings.enable_notifications;
  notif["desktop_notifications"] = settings.desktop_notifications;
  notif["message_alerts"] = settings.message_alerts;
  notif["dnd_hours"] = settings.dnd_hours;

  auto &account = product["account"];
  if (!account.is_object())
    account = tokmon::Json::object();
  account["name"] = settings.account_name;
  account["email"] = settings.account_email;
  account["plan"] = settings.account_plan;
  account["cloud_sync"] = settings.cloud_sync;

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
