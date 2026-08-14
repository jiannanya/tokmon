#pragma once

#include <tokmon/approval.hpp>
#include <tokmon/projection.hpp>
#include <tokmon/snow_client.hpp>
#include <tokmon/product_assembly.hpp>

#include <snow/assembly.hpp>
#include <white/assembly.hpp>

#include <filesystem>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

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
  std::chrono::milliseconds request_timeout{std::chrono::minutes(5)};
  std::string model;
};

class App final {
public:
  explicit App(AppConfig config);
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  int run();
  int smoke();
  void submit(std::string message);
  [[nodiscard]] Projection& projection() noexcept { return *projection_; }
  [[nodiscard]] const tokmon::SessionId& session_id() const noexcept {
    return session_;
  }

private:
  void start_turn(std::string message);
  void connect_child(bool initial);
  void poll_child(std::stop_token stop);
  void supervise_child(std::stop_token stop);
  void apply_events(const tokmon::Json& events);
  void handle_notification(const tokmon::Json& notification);
  void update_status(std::string status);
  void persist_session() const;
  void draw(white::RasterSurface& surface);
  [[nodiscard]] std::shared_ptr<snow::ModelProvider> create_model() const;

  AppConfig config_;
  arche::Runtime ui_runtime_{"tokmon"};
  white::Assembly white_;
  std::shared_ptr<ApprovalCoordinator> approvals_;
  std::shared_ptr<Projection> projection_;
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
  bool turn_active_{false};
  bool restart_requested_{false};
  bool shutting_down_{false};
};

[[nodiscard]] AppConfig load_app_config(
    const std::filesystem::path& workspace,
    std::string config_dir_name = ".tokmon");

} // namespace tokmon::desktop
