#include <snow/tools.hpp>

#include <tokmon/common/files.hpp>
#include <tokmon/common/digest.hpp>

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <set>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace snow {
namespace {

tokmon::Json object_schema(
    std::initializer_list<std::pair<std::string, tokmon::Json>> properties,
    std::initializer_list<std::string> required) {
  tokmon::Json props = tokmon::Json::object();
  for (const auto& [name, value] : properties) {
    props[name] = value;
  }
  return {{"type", "object"},
          {"properties", std::move(props)},
          {"required", required},
          {"additionalProperties", false}};
}

std::string truncate(std::string value, std::size_t maximum) {
  if (value.size() <= maximum) {
    return value;
  }
  value.resize(maximum);
  value += "\n...[truncated]";
  return value;
}

bool matches_type(const tokmon::Json& value, std::string_view type) {
  if (type == "string") return value.is_string();
  if (type == "integer") return value.is_number_integer();
  if (type == "number") return value.is_number();
  if (type == "boolean") return value.is_boolean();
  if (type == "object") return value.is_object();
  if (type == "array") return value.is_array();
  if (type == "null") return value.is_null();
  return false;
}

tokmon::Json validate_and_normalize(const ToolDefinition& definition,
                                    const tokmon::Json& arguments) {
  if (!arguments.is_object()) {
    throw tokmon::Error("snow.tool.schema", "tool arguments must be an object");
  }
  auto normalized = arguments;
  const auto& schema = definition.input_schema;
  const auto properties = schema.value("properties", tokmon::Json::object());
  if (schema.value("additionalProperties", true)) {
    // Explicitly allowed by the contract.
  } else {
    for (const auto& [name, _] : normalized.items()) {
      if (!properties.contains(name)) {
        throw tokmon::Error("snow.tool.schema",
                            "unknown argument '" + name + "' for " +
                                definition.name);
      }
    }
  }
  for (const auto& [name, property] : properties.items()) {
    if (!normalized.contains(name) && property.contains("default")) {
      normalized[name] = property["default"];
    }
  }
  for (const auto& required :
       schema.value("required", std::vector<std::string>{})) {
    if (!normalized.contains(required)) {
      throw tokmon::Error("snow.tool.schema",
                          "missing required argument '" + required + "'");
    }
  }
  for (const auto& [name, value] : normalized.items()) {
    if (!properties.contains(name)) continue;
    const auto type = properties[name].value("type", "");
    if (!type.empty() && !matches_type(value, type)) {
      throw tokmon::Error("snow.tool.schema",
                          "argument '" + name + "' has the wrong type");
    }
  }
  return normalized;
}

#ifdef _WIN32
ToolResult run_windows_process(const std::filesystem::path& workspace,
                               std::string_view command,
                               std::stop_token stop) {
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
      !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
    if (read_pipe) CloseHandle(read_pipe);
    if (write_pipe) CloseHandle(write_pipe);
    return {false, "error", "failed to create process output pipe"};
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  std::wstring command_line = L"cmd.exe /D /S /C \"";
  const auto required = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, command.data(),
      static_cast<int>(command.size()), nullptr, 0);
  if (required <= 0) {
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return {false, "error", "shell command is not valid UTF-8"};
  }
  const auto old_size = command_line.size();
  command_line.resize(old_size + static_cast<std::size_t>(required));
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.data(),
                      static_cast<int>(command.size()),
                      command_line.data() + old_size, required);
  command_line += L"\"";
  std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');

  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
      CloseHandle(job);
      job = nullptr;
    }
  }
  const auto cwd = workspace.wstring();
  const BOOL created = CreateProcessW(
      nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, cwd.c_str(),
      &startup, &process);
  CloseHandle(write_pipe);
  if (!created) {
    CloseHandle(read_pipe);
    if (job) CloseHandle(job);
    return {false, "error", "failed to launch shell process"};
  }
  if (job && !AssignProcessToJobObject(job, process.hProcess)) {
    CloseHandle(job);
    job = nullptr;
  }

  std::string output;
  bool cancelled = false;
  bool limited = false;
  for (;;) {
    if (stop.stop_requested()) {
      cancelled = true;
      if (job) TerminateJobObject(job, 1223);
      else TerminateProcess(process.hProcess, 1223);
    }
    DWORD available = 0;
    if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) &&
        available > 0) {
      std::array<char, 4096> buffer{};
      DWORD read = 0;
      if (ReadFile(read_pipe, buffer.data(),
                   std::min<DWORD>(available, static_cast<DWORD>(buffer.size())),
                   &read, nullptr) && read > 0) {
        const auto room = 1024U * 1024U -
                          std::min<std::size_t>(output.size(), 1024U * 1024U);
        output.append(buffer.data(), std::min<std::size_t>(read, room));
        if (output.size() >= 1024U * 1024U) {
          limited = true;
          if (job) TerminateJobObject(job, 1223);
          else TerminateProcess(process.hProcess, 1223);
        }
      }
    }
    const auto status = WaitForSingleObject(process.hProcess, 20);
    if (status == WAIT_OBJECT_0) {
      for (;;) {
        std::array<char, 4096> buffer{};
        DWORD read = 0;
        if (!ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr) || read == 0) break;
        const auto room = 1024U * 1024U -
                          std::min<std::size_t>(output.size(), 1024U * 1024U);
        output.append(buffer.data(), std::min<std::size_t>(read, room));
      }
      break;
    }
    if (cancelled || limited) continue;
  }
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(read_pipe);
  if (job) CloseHandle(job);
  if (limited) output += "\n...[terminated: output limit exceeded]";
  return {exit_code == 0 && !cancelled && !limited,
          cancelled ? "aborted" : (exit_code == 0 && !limited ? "ok" : "error"),
          std::move(output),
          {{"exit_code", exit_code},
           {"cancelled", cancelled},
           {"output_limited", limited},
           {"process_containment", "windows-job-object"}}};
}
#endif

} // namespace

CanonicalToolPlan normalize_tool_plan(const ToolDefinition& definition,
                                      const tokmon::Json& arguments,
                                      const tokmon::ToolCallId& call_id) {
  CanonicalToolPlan result;
  result.tool = definition.name;
  result.arguments = validate_and_normalize(definition, arguments);
  result.read_only = definition.read_only;
  result.parallel_safe = definition.parallel_safe;
  const tokmon::Json canonical{{"tool", result.tool},
                               {"arguments", result.arguments},
                               {"read_only", result.read_only},
                               {"parallel_safe", result.parallel_safe}};
  result.hash = "sha256:" + tokmon::canonical_json_sha256(canonical);
  result.idempotency_key = "tool:" + call_id.str() + ":" +
                           result.hash.substr(std::string("sha256:").size(), 16);
  return result;
}

void ToolRegistry::add(std::shared_ptr<Tool> tool) {
  if (!tool || tool->definition().name.empty()) {
    throw tokmon::Error("snow.tool.invalid", "tool must have a name");
  }
  std::lock_guard lock(mutex_);
  if (!tools_.emplace(tool->definition().name, std::move(tool)).second) {
    throw tokmon::Error("snow.tool.duplicate", "duplicate tool");
  }
}

void ToolRegistry::remove(std::string_view name) {
  std::lock_guard lock(mutex_);
  tools_.erase(std::string(name));
}

std::shared_ptr<Tool> ToolRegistry::find(std::string_view name) const {
  std::lock_guard lock(mutex_);
  const auto iterator = tools_.find(name);
  return iterator == tools_.end() ? nullptr : iterator->second;
}

std::vector<ToolDefinition> ToolRegistry::definitions() const {
  std::lock_guard lock(mutex_);
  std::vector<ToolDefinition> result;
  result.reserve(tools_.size());
  for (const auto& [_, tool] : tools_) {
    result.push_back(tool->definition());
  }
  return result;
}

tokmon::Json ToolRegistry::model_schema() const {
  tokmon::Json result = tokmon::Json::array();
  for (const auto& definition : definitions()) {
    result.push_back(
        {{"type", "function"},
         {"function",
          {{"name", definition.name},
           {"description", definition.description},
           {"parameters", definition.input_schema}}}});
  }
  return result;
}

ToolDisposition DefaultPolicy::decide(const ToolDefinition& tool,
                                      const tokmon::Json&,
                                      std::string* reason) const {
  if (tool.read_only) {
    if (reason) {
      *reason = "read-only workspace operation";
    }
    return ToolDisposition::allow;
  }
  if (non_interactive_) {
    if (reason) {
      *reason = "non-interactive mode denies mutable tools";
    }
    return ToolDisposition::deny;
  }
  if (reason) {
    *reason = "mutable or process tool requires approval";
  }
  return ToolDisposition::ask;
}

JsonPolicyEngine::JsonPolicyEngine(tokmon::Json policy)
    : policy_(std::move(policy)) {
  if (policy_.value("schema", "") != "org.tokmon.snow.policy/v1")
    throw tokmon::Error("snow.policy.schema", "invalid policy JSON schema");
  if (!policy_.value("defaults", tokmon::Json::object()).is_object() ||
      !policy_.value("tools", tokmon::Json::object()).is_object())
    throw tokmon::Error("snow.policy.schema",
                        "policy defaults and tools must be objects");
  const auto validate = [](std::string_view value) {
    return value == "allow" || value == "ask" || value == "deny";
  };
  for (const auto& [_, value] :
       policy_.value("defaults", tokmon::Json::object()).items())
    if (!value.is_string() || !validate(value.get<std::string>()))
      throw tokmon::Error("snow.policy.decision",
                          "policy decision must be allow, ask, or deny");
  for (const auto& [_, value] :
       policy_.value("tools", tokmon::Json::object()).items())
    if (!value.is_string() || !validate(value.get<std::string>()))
      throw tokmon::Error("snow.policy.decision",
                          "tool policy decision must be allow, ask, or deny");
}

ToolDisposition JsonPolicyEngine::decide(const ToolDefinition& tool,
                                         const tokmon::Json&,
                                         std::string* reason) const {
  const auto tools = policy_.value("tools", tokmon::Json::object());
  std::optional<std::string> selected;
  std::size_t selected_length = 0;
  for (const auto& [pattern, decision] : tools.items()) {
    const auto wildcard = pattern.ends_with('*');
    const auto prefix = wildcard ? pattern.substr(0, pattern.size() - 1)
                                 : pattern;
    const auto matches = wildcard ? tool.name.starts_with(prefix)
                                  : tool.name == pattern;
    if (matches && prefix.size() >= selected_length) {
      selected = decision.get<std::string>();
      selected_length = prefix.size();
    }
  }
  if (!selected) {
    const auto defaults =
        policy_.value("defaults", tokmon::Json::object());
    selected = defaults.value(tool.read_only ? "read_only" : "mutating",
                              tool.read_only ? "allow" : "ask");
  }
  if (reason)
    *reason = "JSON policy selected '" + *selected + "' for " + tool.name;
  if (*selected == "allow") return ToolDisposition::allow;
  if (*selected == "deny") return ToolDisposition::deny;
  return ToolDisposition::ask;
}

ReadFileTool::ReadFileTool(std::filesystem::path workspace)
    : workspace_(std::filesystem::weakly_canonical(std::move(workspace))) {
  definition_ = {
      "read_file",
      "Read a UTF-8 text file inside the workspace",
      object_schema({{"path", {{"type", "string"}}}}, {"path"}),
      true,
      true};
}

ToolResult ReadFileTool::execute(const tokmon::Json& arguments,
                                 std::stop_token stop) {
  if (stop.stop_requested()) {
    return {false, "aborted", "cancelled"};
  }
  const auto path = tokmon::canonical_within(
      workspace_, arguments.at("path").get<std::string>(), true);
  auto content = tokmon::read_text_file(path);
  return {true,
          "ok",
          std::move(content),
          {{"path", std::filesystem::relative(path, workspace_).generic_string()},
           {"bytes", std::filesystem::file_size(path)}}};
}

WriteFileTool::WriteFileTool(std::filesystem::path workspace,
                             std::shared_ptr<ArtifactStore> artifacts)
    : workspace_(std::filesystem::weakly_canonical(std::move(workspace))),
      artifacts_(std::move(artifacts)) {
  definition_ = {
      "write_file",
      "Atomically write a UTF-8 text file inside the workspace",
      object_schema({{"path", {{"type", "string"}}},
                     {"content", {{"type", "string"}}}},
                    {"path", "content"}),
      false,
      false};
}

ToolResult WriteFileTool::execute(const tokmon::Json& arguments,
                                  std::stop_token stop) {
  if (stop.stop_requested()) {
    return {false, "aborted", "cancelled"};
  }
  const auto path = tokmon::canonical_within(
      workspace_, arguments.at("path").get<std::string>());
  const auto content = arguments.at("content").get<std::string>();
  const bool existed = std::filesystem::exists(path);
  std::optional<std::string> before;
  if (existed && std::filesystem::is_regular_file(path)) {
    before = tokmon::read_text_file(path);
  }
  tokmon::Json artifact_metadata = tokmon::Json::object();
  if (before) {
    artifact_metadata["preimage_hash"] =
        "sha256:" + tokmon::sha256_hex(*before);
    if (artifacts_) artifact_metadata["preimage"] = artifacts_->put_text(*before);
  }
  tokmon::write_text_file_atomic(path, content);
  artifact_metadata["postimage_hash"] =
      "sha256:" + tokmon::sha256_hex(content);
  if (artifacts_) artifact_metadata["postimage"] = artifacts_->put_text(content);
  return {true,
          "ok",
          "wrote " + std::to_string(content.size()) + " bytes",
          {{"path", std::filesystem::relative(path, workspace_).generic_string()},
           {"bytes", content.size()},
           {"existed", existed},
           {"artifacts", std::move(artifact_metadata)}}};
}

SearchFilesTool::SearchFilesTool(std::filesystem::path workspace)
    : workspace_(std::filesystem::weakly_canonical(std::move(workspace))) {
  definition_ = {
      "search_files",
      "Search text files recursively in the workspace",
      object_schema({{"query", {{"type", "string"}}},
                     {"path", {{"type", "string"}, {"default", "."}}}},
                    {"query"}),
      true,
      true};
}

ToolResult SearchFilesTool::execute(const tokmon::Json& arguments,
                                    std::stop_token stop) {
  const auto root = tokmon::canonical_within(
      workspace_, arguments.value("path", "."));
  const auto query = arguments.at("query").get<std::string>();
  std::ostringstream output;
  std::size_t matches = 0;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(
           root, std::filesystem::directory_options::skip_permission_denied,
           error),
       end;
       iterator != end && !stop.stop_requested(); iterator.increment(error)) {
    if (error || !iterator->is_regular_file(error) ||
        iterator->file_size(error) > 2 * 1024 * 1024) {
      error.clear();
      continue;
    }
    std::ifstream input(iterator->path());
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
      ++line_number;
      if (line.find(query) != std::string::npos) {
        output << std::filesystem::relative(iterator->path(), workspace_)
                      .generic_string()
               << ':' << line_number << ':' << line << '\n';
        if (++matches >= 1000) {
          break;
        }
      }
    }
    if (matches >= 1000) {
      break;
    }
  }
  return {true,
          stop.stop_requested() ? "aborted" : "ok",
          truncate(output.str(), 512 * 1024),
          {{"matches", matches}, {"truncated", matches >= 1000}}};
}

ShellTool::ShellTool(std::filesystem::path workspace)
    : workspace_(std::filesystem::weakly_canonical(std::move(workspace))) {
  definition_ = {
      "shell",
      "Run a shell command in the workspace",
      object_schema({{"command", {{"type", "string"}}}}, {"command"}),
      false,
      false};
}

ToolResult ShellTool::execute(const tokmon::Json& arguments,
                              std::stop_token stop) {
  if (stop.stop_requested()) {
    return {false, "aborted", "cancelled"};
  }
  const auto command = arguments.at("command").get<std::string>();

#ifdef _WIN32
  auto result = run_windows_process(workspace_, command, stop);
  result.metadata["command"] = command;
  return result;
#else
  const auto quoted_workspace = "'" + workspace_.string() + "'";
  FILE* pipe = popen(("cd " + quoted_workspace + " && " + command + " 2>&1").c_str(), "r");
  if (!pipe) {
    return {false, "error", "failed to launch shell"};
  }
  std::array<char, 4096> buffer{};
  std::string output;
  while (!stop.stop_requested() &&
         std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
    output += buffer.data();
    if (output.size() > 1024 * 1024) {
      break;
    }
  }
  const int code = pclose(pipe);
  return {code == 0 && !stop.stop_requested(),
          stop.stop_requested() ? "aborted" : (code == 0 ? "ok" : "error"),
          truncate(std::move(output), 1024 * 1024),
          {{"exit_code", code}, {"command", command}}};
#endif
}

} // namespace snow
