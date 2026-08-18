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

// Editable product settings for Tokmon Desktop 2.1. Secrets are referenced through environment
// variable names and are never persisted in the workspace JSON documents.
struct DesktopSettings {
  std::string config_dir_name{".tokmon"};

  // 1. 通用 (General)
  std::string language{"zh-CN"}; // 简体中文
  std::string open_on_startup{"home"}; // home (首页) / last_session (上次打开的会话)
  std::string auto_save_interval{"5 分钟"}; // 5 分钟
  std::string update_channel{"stable"}; // stable (稳定版) / beta (测试版)

  // 2. 智能体与模型 (Agents & Models)
  std::string default_agent{"代码助手"}; // 默认智能体
  std::string provider_mode{"official"}; // official (Tokmon 官方) / custom (自定义)
  std::string provider_id{"default"};
  std::string provider_name{"Default provider"};
  std::string provider_kind{"openai-compatible"};
  std::string endpoint{"https://api.openai.com/v1/chat/completions"};
  std::string api_key_env{"OPENAI_API_KEY"};
  std::string model{"faster-whisper-large-v3-turbo"};
  std::string reasoning_effort{"standard"}; // low (低) / standard (标准) / high (高)
  std::string request_timeout_ms{"300000"};
  std::string agent_preset{"balanced"};
  std::string max_steps{"32"};

  // 3. 权限与安全 (Security & Permissions)
  std::string file_access{"trusted"}; // trusted (受信路径) / all (全部允许) / ask (按需询问)
  std::string command_approval{"on_demand"}; // auto (自动执行) / on_demand (按需确认) / deny (禁止执行)
  bool network_access{true}; // 网络访问
  bool high_risk_confirm{true}; // 高风险二次确认
  std::string default_permission{"ask"};

  // 4. 工作区 (Workspace)
  std::string default_workspace{"C:\\Users\\User\\Tokmon\\Projects"};
  std::string index_mode{"standard"}; // standard (标准) / deep (深度)
  bool auto_sync{true}; // 自动同步
  bool git_integration{true}; // Git 集成

  // 5. 通知 (Notifications)
  bool enable_notifications{true}; // 启用通知
  bool desktop_notifications{true}; // 桌面通知
  bool message_alerts{true}; // 消息提醒
  std::string dnd_hours{"22:00 - 08:00"}; // 免打扰时间

  // 6. 外观 (Appearance)
  std::string theme{"light"}; // light (浅色) / dark (深色)
  std::string accent_color{"gold"}; // gold (浅金色) / coral / purple / blue / green / grey
  std::string ui_density{"comfortable"}; // compact (紧凑) / comfortable (舒适) / loose (宽松)
  int font_size_percent{100}; // 字体大小 100%

  // 7. 快捷键 (Shortcuts)
  std::string shortcut_preset{"Tokmon 默认"};
  int modified_shortcuts_count{0};
  std::string shortcut_conflict_status{"无冲突"};

  // 8. 账户 (Account)
  std::string account_name{"Jiandong Chen"};
  std::string account_email{"jiandong.chen@tokmon.ai"};
  std::string account_plan{"Pro"};
  bool cloud_sync{true}; // 云同步

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
