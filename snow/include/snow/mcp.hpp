#pragma once

#include <snow/tools.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace snow {

struct McpServerConfig {
  std::string id;
  std::filesystem::path command;
  std::vector<std::string> arguments;
  std::filesystem::path cwd;
  std::map<std::string, std::string, std::less<>> environment;
  std::chrono::milliseconds request_timeout{30000};
  bool enabled{true};

  [[nodiscard]] static McpServerConfig parse(
      const tokmon::Json& document,
      const std::filesystem::path& workspace);
};

// One isolated stdio MCP server. The provider owns the process and all tools
// returned from tools/list retain it through a shared lease.
class McpToolProvider final {
public:
  class Impl;

  explicit McpToolProvider(McpServerConfig config);
  ~McpToolProvider();

  McpToolProvider(const McpToolProvider&) = delete;
  McpToolProvider& operator=(const McpToolProvider&) = delete;

  [[nodiscard]] const std::string& id() const noexcept;
  [[nodiscard]] const std::vector<std::shared_ptr<Tool>>& tools() const;
  [[nodiscard]] tokmon::Json diagnostics() const;
  void stop() noexcept;

private:
  std::shared_ptr<Impl> impl_;
  std::vector<std::shared_ptr<Tool>> tools_;
};

[[nodiscard]] std::vector<McpServerConfig> load_mcp_config(
    const std::filesystem::path& mcp_json,
    const std::filesystem::path& workspace);

} // namespace snow
