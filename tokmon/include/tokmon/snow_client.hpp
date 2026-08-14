#pragma once

#include <tokmon/common/types.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tokmon::desktop {

struct SnowProcessOptions {
  std::filesystem::path executable;
  std::filesystem::path workspace;
  std::filesystem::path data_root;
  std::string config_dir_name{".tokmon"};
  bool raw_trace{false};
  std::chrono::milliseconds request_timeout{std::chrono::minutes(5)};
};

struct SnowProcessExit {
  std::uint32_t code{0};
  bool expected{false};
  std::vector<std::string> diagnostics;
};

// Owns exactly one Snow stdio server. Requests may be issued concurrently;
// responses are correlated by JSON-RPC id and notifications are delivered on
// the reader thread, so handlers must remain non-blocking.
class SnowProcessClient final {
public:
  using NotificationHandler = std::function<void(const tokmon::Json&)>;
  using CrashHandler = std::function<void(const SnowProcessExit&)>;

  explicit SnowProcessClient(SnowProcessOptions options);
  ~SnowProcessClient();
  SnowProcessClient(const SnowProcessClient&) = delete;
  SnowProcessClient& operator=(const SnowProcessClient&) = delete;

  void start();
  void stop();
  [[nodiscard]] bool alive() const;
  [[nodiscard]] std::uint32_t process_id() const;

  [[nodiscard]] tokmon::Json request(
      std::string_view method,
      tokmon::Json params = tokmon::Json::object(),
      std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
  [[nodiscard]] tokmon::Json initialize();

  void set_notification_handler(NotificationHandler handler);
  void set_crash_handler(CrashHandler handler);
  [[nodiscard]] std::vector<std::string> diagnostics() const;

  [[nodiscard]] static std::filesystem::path sibling_snow_executable();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace tokmon::desktop
