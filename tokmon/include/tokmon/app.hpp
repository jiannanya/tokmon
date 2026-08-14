#pragma once

#include <tokmon/approval.hpp>
#include <tokmon/product_assembly.hpp>
#include <tokmon/projection.hpp>
#include <tokmon/settings.hpp>
#include <tokmon/snow_client.hpp>
#include <tokmon/workbench.hpp>

#include <snow/assembly.hpp>
#include <white/assembly.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace tokmon::desktop {

struct AppConfig {
  std::filesystem::path workspace{std::filesystem::current_path()};
  std::filesystem::path data_root;
  std::string config_dir_name{".tokmon"};
  std::filesystem::path snow_executable;
  bool embedded_snow{false};
  bool raw_trace{false};
  bool restart_enabled{true};
  std::size_t restart_max_attempts{5};
  std::chrono::milliseconds restart_base_delay{250};
  std::chrono::milliseconds poll_interval{25};
  float ui_scale{1.25F};
  std::chrono::milliseconds request_timeout{std::chrono::minutes(5)};
  std::string model;
  std::size_t max_steps{32};
};

class App final {
public:
  explicit App(AppConfig config);
  ~App();
  App(const App &) = delete;
  App &operator=(const App &) = delete;

  int run();
  int smoke();
  void capture(const std::filesystem::path &path);
  void submit(std::string message);
  [[nodiscard]] Projection &projection() noexcept { return *projection_; }
  [[nodiscard]] const tokmon::SessionId &session_id() const noexcept {
    return session_;
  }

private:
  void start_turn(std::string message, tokmon::Json attachments);
  void connect_child(bool initial);
  void poll_child(std::stop_token stop);
  void supervise_child(std::stop_token stop);
  void apply_events(const tokmon::Json &events);
  void handle_notification(const tokmon::Json &notification);
  void update_status(std::string status);
  [[nodiscard]] bool handle_workbench_event(const white::UiEvent &event);
  void handle_editor_submit(std::string value);
  void refresh_sessions();
  void switch_session(std::string session_id);
  void choose_attachments();
  void choose_document();
  enum class InputMode { message, filter, trajectory_search, settings };
  void set_input_mode(InputMode mode, std::string settings_field = {});
  void apply_setting(std::string value, std::size_t index);
  void export_trajectory();
  void persist_session() const;
  void draw(white::RasterSurface &surface);
  [[nodiscard]] std::shared_ptr<snow::ModelProvider> create_model() const;

  AppConfig config_;
  WorkbenchView workbench_;
  arche::Runtime ui_runtime_{"tokmon"};
  white::Assembly white_;
  std::shared_ptr<ApprovalCoordinator> approvals_;
  std::shared_ptr<Projection> projection_;
  std::vector<ConversationItem> cached_projection_items_;
  std::vector<snow::TrajectoryEvent> cached_projection_events_;
  std::uint64_t cached_projection_cursor_{0};
  std::uint64_t cached_projection_revision_{0};
  std::vector<WorkbenchSession> cached_sessions_;
  std::uint64_t cached_sessions_revision_{0};
  std::unique_ptr<snow::Assembly> embedded_snow_;
  std::shared_ptr<SnowProcessClient> snow_process_;
  std::unique_ptr<ProductAssembly> product_;
  tokmon::SessionId session_;
  axon::Connection event_connection_;
  std::unique_ptr<white::Window> window_;
  std::jthread active_turn_;
  std::jthread poller_;
  std::jthread supervisor_;
  mutable std::mutex state_mutex_;
  std::condition_variable_any supervisor_ready_;
  std::string status_{"Ready"};
  struct AttachedFile {
    std::filesystem::path path;
    std::string name;
    std::string content;
    std::string sha256;
  };
  std::vector<WorkbenchSession> sessions_;
  std::uint64_t sessions_revision_{0};
  std::vector<AttachedFile> attachments_;
  std::string message_draft_;
  std::string file_filter_;
  std::string trajectory_search_;
  DesktopSettings settings_;
  std::string active_settings_field_;
  InputMode input_mode_{InputMode::message};
  bool turn_active_{false};
  bool restart_requested_{false};
  bool shutting_down_{false};
};

[[nodiscard]] AppConfig
load_app_config(const std::filesystem::path &workspace,
                std::string config_dir_name = ".tokmon");

} // namespace tokmon::desktop
