#include <snow/c_api.h>

#include <snow/assembly.hpp>
#include <snow/model.hpp>
#include <snow/protocol.hpp>

#include <tokmon/common/files.hpp>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

namespace {

thread_local std::string last_code;
thread_local std::string last_message;

void clear_error() {
  last_code.clear();
  last_message.clear();
}

snow_status_v1 fail(std::string code, std::string message,
                    snow_status_v1 status = SNOW_STATUS_ERROR_V1) {
  last_code = std::move(code);
  last_message = std::move(message);
  return status;
}

tokmon::Json parse(std::string_view input, std::string_view code) {
  try {
    return tokmon::Json::parse(input, nullptr, true, false);
  } catch (const nlohmann::json::exception& error) {
    throw tokmon::Error(std::string(code), error.what());
  }
}

struct ModelAndSecrets {
  std::shared_ptr<snow::ModelProvider> model;
  std::vector<std::string> secrets;
};

ModelAndSecrets create_model(const snow::BootstrapConfig& config) {
  const auto layout = snow::ConfigLayout::resolve(
      config.workspace, config.config_dir_name);
  const auto document = snow::load_providers_config(layout);
  const auto provider = document.value("default", tokmon::Json::object());
  const auto endpoint = provider.value("endpoint", "");
  if (!endpoint.empty()) {
    std::string key;
    if (const auto value = tokmon::environment_variable(
            provider.value("api_key_env", "")))
      key = *value;
    ModelAndSecrets result;
    if (!key.empty()) result.secrets.push_back(key);
    result.model = std::make_shared<snow::OpenAICompatibleProvider>(
        snow::OpenAICompatibleConfig{
            .endpoint = endpoint,
            .api_key = std::move(key),
            .model = provider.value("model", "")});
    return result;
  }
  return {std::make_shared<snow::ScriptedModelProvider>(
              std::vector<snow::ModelResponse>{{
                  .content = "No model provider is configured in " +
                             layout.providers.generic_string()}}),
          {}};
}

} // namespace

struct snow_host_v1 {
  explicit snow_host_v1(snow::BootstrapConfig config,
                        std::shared_ptr<snow::ModelProvider> model)
      : assembly(std::move(config), std::move(model)) {
    auto artifacts = assembly.arche_runtime().root_context()->require<
        snow::ArtifactStore>("snow.artifacts", "^1.0");
    server = std::make_unique<snow::ProtocolServer>(
        assembly.agent(), nullptr, artifacts.shared(),
        &assembly.arche_runtime(), &assembly);
  }

  snow::Assembly assembly;
  std::unique_ptr<snow::ProtocolServer> server;
  std::mutex invoke_mutex;
};

extern "C" {

uint32_t snow_abi_version_v1(void) { return SNOW_C_ABI_VERSION_V1; }

snow_status_v1 snow_host_create_v1(const char* bootstrap_json,
                                   size_t bootstrap_size,
                                   snow_host_v1** out_host) {
  clear_error();
  if (!bootstrap_json || !out_host)
    return fail("snow.c.invalid-argument", "null create argument",
                SNOW_STATUS_INVALID_ARGUMENT_V1);
  *out_host = nullptr;
  try {
    const auto document = parse(
        std::string_view(bootstrap_json, bootstrap_size),
        "snow.c.bootstrap-json");
    if (document.value("schema", "") != "org.tokmon.snow.bootstrap/v1")
      throw tokmon::Error("snow.c.bootstrap-schema",
                          "invalid Snow bootstrap schema");
    snow::BootstrapConfig config;
    config.workspace = document.at("workspace").get<std::string>();
    config.config_dir_name = document.value("config_dir_name", ".snow");
    if (document.contains("data_root"))
      config.data_root = document.at("data_root").get<std::string>();
    config.max_steps = document.value("max_steps", 32U);
    config.max_context_chars =
        document.value("max_context_chars", 512U * 1024U);
    config.max_tool_result_bytes =
        document.value("max_tool_result_bytes", 256U * 1024U);
    config.raw_trace_enabled = document.value("raw_trace", false);
    auto model = create_model(config);
    config.sensitive_values = std::move(model.secrets);
    *out_host = new snow_host_v1(std::move(config), std::move(model.model));
    return SNOW_STATUS_OK_V1;
  } catch (const tokmon::Error& error) {
    return fail(error.code(), error.what());
  } catch (const std::exception& error) {
    return fail("snow.c.create", error.what());
  } catch (...) {
    return fail("snow.c.create", "unknown Snow host creation failure");
  }
}

void snow_host_destroy_v1(snow_host_v1* host) { delete host; }

snow_status_v1 snow_host_invoke_v1(snow_host_v1* host,
                                   const char* request_json,
                                   size_t request_size,
                                   snow_buffer_v1* out_response) {
  clear_error();
  if (!host || !request_json || !out_response)
    return fail("snow.c.invalid-argument", "null invoke argument",
                SNOW_STATUS_INVALID_ARGUMENT_V1);
  out_response->data = nullptr;
  out_response->size = 0;
  try {
    const auto request = parse(std::string_view(request_json, request_size),
                               "snow.c.request-json");
    // ProtocolServer owns its internal active-run synchronization. The lock
    // only protects adapters that are not otherwise concurrently mutable;
    // turn.start/cancel must be allowed to overlap, so those calls bypass it.
    tokmon::Json response;
    const auto method = request.value("method", "");
    try {
      if (method == "turn.start" || method == "turn.cancel" ||
          method == "turn.steer") {
        response = host->server->handle(request);
      } else {
        std::lock_guard lock(host->invoke_mutex);
        response = host->server->handle(request);
      }
    } catch (const tokmon::Error& error) {
      response = {{"jsonrpc", "2.0"},
                  {"id", request.value("id", tokmon::Json(nullptr))},
                  {"error", {{"code", error.code()},
                             {"message", error.what()},
                             {"data", error.details()}}}};
    } catch (const std::exception& error) {
      response = {{"jsonrpc", "2.0"},
                  {"id", request.value("id", tokmon::Json(nullptr))},
                  {"error", {{"code", "internal"},
                             {"message", error.what()}}}};
    }
    const auto encoded = response.dump();
    auto* memory = static_cast<char*>(std::malloc(encoded.size() + 1));
    if (!memory) throw std::bad_alloc();
    std::memcpy(memory, encoded.data(), encoded.size());
    memory[encoded.size()] = '\0';
    out_response->data = memory;
    out_response->size = encoded.size();
    return SNOW_STATUS_OK_V1;
  } catch (const tokmon::Error& error) {
    return fail(error.code(), error.what());
  } catch (const std::exception& error) {
    return fail("snow.c.invoke", error.what());
  } catch (...) {
    return fail("snow.c.invoke", "unknown Snow invocation failure");
  }
}

void snow_buffer_release_v1(snow_buffer_v1* buffer) {
  if (!buffer) return;
  std::free(buffer->data);
  buffer->data = nullptr;
  buffer->size = 0;
}

const char* snow_last_error_code_v1(void) { return last_code.c_str(); }
const char* snow_last_error_message_v1(void) { return last_message.c_str(); }

} // extern "C"
