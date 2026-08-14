#include <snow/config.hpp>

#include <tokmon/common/files.hpp>

#include <regex>

namespace snow {

ConfigLayout ConfigLayout::resolve(std::filesystem::path workspace,
                                   std::string config_dir_name) {
  if (config_dir_name.empty() || config_dir_name == "." ||
      config_dir_name == ".." ||
      config_dir_name.find('/') != std::string::npos ||
      config_dir_name.find('\\') != std::string::npos ||
      std::filesystem::path(config_dir_name).is_absolute()) {
    throw tokmon::Error("snow.config_dir_name",
                        "config_dir_name must be one safe directory name");
  }
  static const std::regex valid(R"(^[A-Za-z0-9._-]+$)");
  if (!std::regex_match(config_dir_name, valid)) {
    throw tokmon::Error("snow.config_dir_name",
                        "config_dir_name contains unsupported characters");
  }

  ConfigLayout result;
  result.workspace = std::filesystem::weakly_canonical(workspace);
  result.config_dir_name = std::move(config_dir_name);
  result.config_root =
      tokmon::canonical_within(result.workspace, result.config_dir_name);
  result.instructions = result.config_root / "instructions.md";
  result.skills = result.config_root / "skills";
  result.memory = result.config_root / "memory";
  result.mcp = result.config_root / "mcp.json";
  result.providers = result.config_root / "providers.json";
  result.policy = result.config_root / "policy.json";
  result.composition = result.config_root / "composition.json";
  return result;
}

tokmon::Json load_json_config(const std::filesystem::path& path,
                              tokmon::Json fallback) {
  if (!std::filesystem::exists(path)) {
    return fallback;
  }
  try {
    return tokmon::Json::parse(tokmon::read_text_file(path), nullptr, true,
                               false);
  } catch (const nlohmann::json::exception& error) {
    throw tokmon::Error("snow.config.json",
                        "invalid JSON in " + path.string() + ": " +
                            error.what());
  }
}

tokmon::Json load_providers_config(const ConfigLayout& layout) {
  if (!std::filesystem::exists(layout.providers)) return tokmon::Json::object();
  const auto document = load_json_config(layout.providers);
  if (document.value("schema", "") != "org.tokmon.snow.providers/v1")
    throw tokmon::Error("snow.providers.schema",
                        "invalid providers.json schema");
  const auto configured =
      document.value("default", tokmon::Json::object());
  if (!configured.is_object())
    throw tokmon::Error("snow.providers.default",
                        "providers.default must be an object");
  if (configured.contains("api_key"))
    throw tokmon::Error(
        "snow.providers.secret",
        "providers.json must reference api_key_env, not store api_key");
  if (!configured.empty() &&
      configured.value("kind", "openai-compatible") !=
          "openai-compatible")
    throw tokmon::Error("snow.providers.kind",
                        "unsupported model provider kind");
  return document;
}

} // namespace snow
