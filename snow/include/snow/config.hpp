#pragma once

#include <tokmon/common/types.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace snow {

struct ConfigLayout {
  std::filesystem::path workspace;
  std::string config_dir_name{".snow"};
  std::filesystem::path config_root;
  std::filesystem::path instructions;
  std::filesystem::path skills;
  std::filesystem::path memory;
  std::filesystem::path mcp;
  std::filesystem::path providers;
  std::filesystem::path policy;
  std::filesystem::path composition;

  [[nodiscard]] static ConfigLayout resolve(
      std::filesystem::path workspace,
      std::string config_dir_name = ".snow");
};

struct BootstrapConfig {
  std::filesystem::path workspace{std::filesystem::current_path()};
  std::string config_dir_name{".snow"};
  std::filesystem::path data_root;
  std::size_t max_steps{32};
  std::size_t max_context_chars{512 * 1024};
  std::size_t max_tool_result_bytes{256 * 1024};
  bool raw_trace_enabled{false};
  // Runtime-only secret material used solely for irreversible redaction before
  // semantic trajectory commit. It is never serialized into configuration.
  std::vector<std::string> sensitive_values;
};

[[nodiscard]] tokmon::Json load_json_config(
    const std::filesystem::path& path,
    tokmon::Json fallback = tokmon::Json::object());

[[nodiscard]] tokmon::Json load_providers_config(
    const ConfigLayout& layout);

} // namespace snow
