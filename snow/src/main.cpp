#include <snow/assembly.hpp>
#include <snow/mcp.hpp>
#include <snow/protocol.hpp>

#include <arche/composition.hpp>
#include <arche/package.hpp>

#include <tokmon/common/files.hpp>

#include <iostream>

namespace {

std::string argument(int argc, char** argv, std::string_view key,
                     std::string fallback = {}) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (argv[index] == key) {
      return argv[index + 1];
    }
  }
  return fallback;
}

bool flag(int argc, char** argv, std::string_view key) {
  for (int index = 1; index < argc; ++index) {
    if (argv[index] == key) return true;
  }
  return false;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const std::string command = argc > 1 ? argv[1] : "doctor";
    snow::BootstrapConfig config;
    config.workspace = argument(argc, argv, "--workspace",
                                std::filesystem::current_path().string());
    config.config_dir_name =
        argument(argc, argv, "--config-dir-name", ".snow");
    config.data_root = argument(
        argc, argv, "--data-root",
        (config.workspace / config.config_dir_name / "data").string());
    config.raw_trace_enabled = flag(argc, argv, "--raw-trace");

    if (command == "doctor") {
      const auto layout = snow::ConfigLayout::resolve(
          config.workspace, config.config_dir_name);
      tokmon::Json checks = tokmon::Json::array();
      (void)snow::load_providers_config(layout);
      checks.push_back({{"name", "providers"}, {"status", "ok"}});
      (void)snow::load_mcp_config(layout.mcp, layout.workspace);
      checks.push_back({{"name", "mcp"}, {"status", "ok"}});
      if (std::filesystem::exists(layout.policy))
        (void)snow::JsonPolicyEngine(snow::load_json_config(layout.policy));
      checks.push_back({{"name", "policy"}, {"status", "ok"}});
      if (std::filesystem::exists(layout.composition))
        (void)arche::DesiredComposition::parse(
            snow::load_json_config(layout.composition));
      checks.push_back({{"name", "composition"}, {"status", "ok"}});
      const auto trust = layout.config_root / "trust.json";
      if (std::filesystem::exists(trust))
        (void)arche::PackageTrustStore(trust);
      checks.push_back({{"name", "trust"}, {"status", "ok"}});
      std::cout << tokmon::Json{
                       {"status", "ok"},
                       {"workspace", layout.workspace.generic_string()},
                       {"config_dir_name", layout.config_dir_name},
                       {"config_root", layout.config_root.generic_string()},
                       {"data_root", config.data_root.generic_string()},
                       {"checks", std::move(checks)}}
                       .dump(2)
                << '\n';
      return 0;
    }

    std::shared_ptr<snow::ModelProvider> model;
    const auto layout =
        snow::ConfigLayout::resolve(config.workspace, config.config_dir_name);
    const auto providers = snow::load_providers_config(layout);
    const auto configured = providers.value("default", tokmon::Json::object());
    const auto endpoint = argument(
        argc, argv, "--endpoint", configured.value("endpoint", ""));
    if (!endpoint.empty()) {
      snow::OpenAICompatibleConfig provider;
      provider.endpoint = endpoint;
      provider.api_key = argument(argc, argv, "--api-key");
      if (provider.api_key.empty()) {
        const auto key_env = configured.value("api_key_env", "");
        if (const auto value = tokmon::environment_variable(key_env))
          provider.api_key = *value;
      }
      if (!provider.api_key.empty())
        config.sensitive_values.push_back(provider.api_key);
      provider.model = argument(argc, argv, "--model",
                                configured.value("model", ""));
      model = std::make_shared<snow::OpenAICompatibleProvider>(
          std::move(provider));
    } else {
      model = std::make_shared<snow::ScriptedModelProvider>(
          std::vector<snow::ModelResponse>{
              {.content = "Snow is running without a configured remote model."}});
    }
    std::shared_ptr<snow::ApprovalService> approvals;
    std::shared_ptr<snow::ProtocolApprovalService> protocol_approvals;
    if (command == "serve") {
      protocol_approvals = std::make_shared<snow::ProtocolApprovalService>();
      approvals = protocol_approvals;
    } else {
      approvals = std::make_shared<snow::CallbackApproval>(
          [](const snow::ToolDefinition& tool, const tokmon::Json& arguments,
             std::string_view reason) {
            std::cerr << "Approve " << tool.name << " " << arguments.dump()
                      << " (" << reason << ")? [y/N] ";
            std::string answer;
            std::getline(std::cin, answer);
            return answer == "y" || answer == "Y";
          });
    }
    snow::Assembly assembly(config, std::move(model), std::move(approvals));

    if (command == "serve") {
      auto artifacts = assembly.arche_runtime().root_context()
                           ->require<snow::ArtifactStore>("snow.artifacts",
                                                          "^1.0");
      snow::ProtocolServer server(assembly.agent(), protocol_approvals,
                                  artifacts.shared(),
                                  &assembly.arche_runtime(), &assembly);
      server.serve(std::cin, std::cout);
      return 0;
    }
    if (command == "run") {
      const auto message = argument(argc, argv, "--message");
      if (message.empty()) {
        std::cerr << "snow run requires --message\n";
        return 2;
      }
      const auto session = assembly.agent().create_session();
      const auto result = assembly.agent().run(
          session, message,
          {.model = argument(argc, argv, "--model"), .max_steps = 32});
      std::cout << result.final_text << '\n';
      return result.reason == snow::TurnEndReason::completed ? 0 : 1;
    }
    std::cerr << "usage: snow doctor|serve|run [options]\n";
    return 2;
  } catch (const tokmon::Error& error) {
    std::cerr << error.code() << ": " << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
