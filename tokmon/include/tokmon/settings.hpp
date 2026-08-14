#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tokmon::desktop {

struct PluginSetting {
  std::string instance;
  std::string package;
  std::string realm;
  bool disabled{false};
  bool required{false};
};

// Editable product settings. Secrets are referenced through environment
// variable names and are never persisted in the workspace JSON documents.
struct DesktopSettings {
  std::string config_dir_name{".tokmon"};
  std::string language{"zh-CN"};
  std::string theme{"system"};
  std::string provider_id{"default"};
  std::string provider_name{"Default provider"};
  std::string provider_kind{"openai-compatible"};
  std::string endpoint{"https://api.openai.com/v1/chat/completions"};
  std::string api_key_env{"OPENAI_API_KEY"};
  std::string model{"gpt-5"};
  std::string request_timeout_ms{"300000"};
  std::string agent_preset{"balanced"};
  std::string max_steps{"32"};
  std::string default_permission{"ask"};
  bool raw_trace{false};
  bool restart_enabled{true};
  bool auto_scroll{true};
  std::vector<PluginSetting> plugins;
};

[[nodiscard]] DesktopSettings
load_desktop_settings(const std::filesystem::path &workspace,
                      std::string config_dir_name = ".tokmon");

void save_desktop_settings(const std::filesystem::path &workspace,
                           const DesktopSettings &settings);

} // namespace tokmon::desktop
