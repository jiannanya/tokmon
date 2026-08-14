#include <snow/mcp.hpp>

#include <snow/config.hpp>

#include <tokmon/common/files.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cwchar>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace snow {
namespace {

std::string safe_component(std::string value) {
  for (auto& character : value) {
    const auto safe =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '_' ||
        character == '-';
    if (!safe) character = '_';
  }
  if (value.empty()) value = "unnamed";
  return value;
}

#ifdef _WIN32
std::wstring widen(std::string_view value) {
  if (value.empty()) return {};
  const auto size = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0)
    throw tokmon::Error("snow.mcp.utf8", "MCP argument is not valid UTF-8");
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size);
  return result;
}

std::wstring quote_argument(const std::wstring& value) {
  if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
  std::wstring result(1, L'"');
  std::size_t slashes = 0;
  for (const auto character : value) {
    if (character == L'\\') {
      ++slashes;
    } else if (character == L'"') {
      result.append(slashes * 2 + 1, L'\\');
      result.push_back(L'"');
      slashes = 0;
    } else {
      result.append(slashes, L'\\');
      slashes = 0;
      result.push_back(character);
    }
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

std::vector<wchar_t> child_environment(
    const std::map<std::string, std::string, std::less<>>& overrides) {
  std::vector<std::wstring> entries;
  if (auto* block = GetEnvironmentStringsW()) {
    for (auto* cursor = block; *cursor; cursor += std::wcslen(cursor) + 1)
      entries.emplace_back(cursor);
    FreeEnvironmentStringsW(block);
  }
  for (const auto& [name, value] : overrides) {
    const auto prefix = widen(name) + L"=";
    std::erase_if(entries, [&](const auto& entry) {
      return entry.size() >= prefix.size() &&
             _wcsnicmp(entry.c_str(), prefix.c_str(), prefix.size()) == 0;
    });
    entries.push_back(prefix + widen(value));
  }
  std::ranges::sort(entries, [](const auto& left, const auto& right) {
    return _wcsicmp(left.c_str(), right.c_str()) < 0;
  });
  std::vector<wchar_t> result;
  for (const auto& entry : entries) {
    result.insert(result.end(), entry.begin(), entry.end());
    result.push_back(L'\0');
  }
  result.push_back(L'\0');
  if (result.size() == 1) result.push_back(L'\0');
  return result;
}
#endif

} // namespace

McpServerConfig McpServerConfig::parse(
    const tokmon::Json& document,
    const std::filesystem::path& workspace) {
  if (!document.is_object())
    throw tokmon::Error("snow.mcp.config", "MCP server must be an object");
  McpServerConfig result;
  result.id = document.at("id").get<std::string>();
  if (result.id.empty() || safe_component(result.id) != result.id)
    throw tokmon::Error(
        "snow.mcp.id",
        "MCP server id must contain only letters, digits, '_' or '-'");
  result.command = document.at("command").get<std::string>();
  if (result.command.empty())
    throw tokmon::Error("snow.mcp.command", "MCP command is empty");
  result.arguments =
      document.value("args", std::vector<std::string>{});
  result.cwd = document.contains("cwd")
                   ? tokmon::canonical_within(
                         workspace, document.at("cwd").get<std::string>(),
                         true)
                   : std::filesystem::weakly_canonical(workspace);
  const auto timeout = document.value("request_timeout_ms", 30000LL);
  if (timeout < 100 || timeout > 600000)
    throw tokmon::Error("snow.mcp.timeout",
                        "MCP request_timeout_ms is outside 100..600000");
  result.request_timeout = std::chrono::milliseconds(timeout);
  result.enabled = document.value("enabled", true);
  if (document.contains("env")) {
    // Literal secrets do not belong in mcp.json. Environment inheritance is
    // the only supported secret injection path in the in-process host.
    const auto& environment = document.at("env");
    if (!environment.is_object())
      throw tokmon::Error("snow.mcp.env", "MCP env must be an object");
    for (const auto& [name, source] : environment.items()) {
      if (!source.is_object() || source.value("from_env", "").empty() ||
          source.size() != 1)
        throw tokmon::Error(
            "snow.mcp.env",
            "MCP env values must only contain a from_env reference",
            {{"name", name}});
      const auto source_name = source.at("from_env").get<std::string>();
      const auto value = tokmon::environment_variable(source_name);
      if (!value)
        throw tokmon::Error("snow.mcp.env-missing",
                            "referenced MCP environment variable is missing",
                            {{"name", name}, {"from_env", source_name}});
      result.environment[name] = *value;
    }
  }
  return result;
}

class McpToolProvider::Impl final {
public:
  struct Pending {
    std::mutex mutex;
    std::condition_variable ready;
    bool completed{false};
    tokmon::Json response;
  };

  explicit Impl(McpServerConfig value) : config(std::move(value)) {}
  ~Impl() { stop(); }

  McpServerConfig config;
  mutable std::mutex state_mutex;
  std::mutex write_mutex;
  std::map<std::uint64_t, std::shared_ptr<Pending>> pending;
  std::vector<std::string> stderr_lines;
  std::atomic_uint64_t next_id{1};
  std::atomic_bool stopping{false};
  std::jthread output_reader;
  std::jthread error_reader;
#ifdef _WIN32
  HANDLE process{nullptr};
  HANDLE job{nullptr};
  HANDLE input_write{nullptr};
  HANDLE output_read{nullptr};
  HANDLE error_read{nullptr};
  DWORD pid{0};
#else
  pid_t pid{-1};
  int input_write{-1};
  int output_read{-1};
  int error_read{-1};
#endif

  void start() {
    if (alive()) return;
    stopping = false;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE input_read = nullptr;
    HANDLE output_write = nullptr;
    HANDLE error_write = nullptr;
    auto cleanup = [&] {
      if (input_read) CloseHandle(input_read);
      if (output_write) CloseHandle(output_write);
      if (error_write) CloseHandle(error_write);
    };
    if (!CreatePipe(&input_read, &input_write, &security, 0) ||
        !CreatePipe(&output_read, &output_write, &security, 0) ||
        !CreatePipe(&error_read, &error_write, &security, 0)) {
      cleanup();
      throw tokmon::Error("snow.mcp.pipe", "failed to create MCP pipes");
    }
    SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(error_read, HANDLE_FLAG_INHERIT, 0);
    auto executable = config.command;
    if (executable.is_absolute())
      executable = std::filesystem::weakly_canonical(executable);
    std::wstring command = quote_argument(executable.wstring());
    for (const auto& argument : config.arguments)
      command += L" " + quote_argument(widen(argument));
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input_read;
    startup.hStdOutput = output_write;
    startup.hStdError = error_write;
    PROCESS_INFORMATION information{};
    const auto environment = child_environment(config.environment);
    const auto created = CreateProcessW(
        executable.is_absolute() ? executable.c_str() : nullptr,
        mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        const_cast<wchar_t*>(environment.data()),
        config.cwd.c_str(), &startup, &information);
    cleanup();
    if (!created) {
      CloseHandle(input_write);
      CloseHandle(output_read);
      CloseHandle(error_read);
      input_write = output_read = error_read = nullptr;
      throw tokmon::Error("snow.mcp.launch", "failed to launch MCP server",
                          {{"server", config.id},
                           {"command", config.command.generic_string()},
                           {"windows_error", GetLastError()}});
    }
    CloseHandle(information.hThread);
    process = information.hProcess;
    pid = information.dwProcessId;
    job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
      TerminateProcess(process, 1);
      throw tokmon::Error("snow.mcp.job", "failed to create MCP job object");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                            sizeof(limits));
    if (!AssignProcessToJobObject(job, process)) {
      TerminateProcess(process, 1);
      throw tokmon::Error("snow.mcp.job",
                          "failed to contain MCP server in job object");
    }
#else
    int input_pipe[2]{};
    int output_pipe[2]{};
    int error_pipe[2]{};
    if (pipe(input_pipe) || pipe(output_pipe) || pipe(error_pipe))
      throw tokmon::Error("snow.mcp.pipe", "failed to create MCP pipes");
    pid = fork();
    if (pid == 0) {
      dup2(input_pipe[0], STDIN_FILENO);
      dup2(output_pipe[1], STDOUT_FILENO);
      dup2(error_pipe[1], STDERR_FILENO);
      chdir(config.cwd.c_str());
      for (const auto& [name, value] : config.environment)
        setenv(name.c_str(), value.c_str(), 1);
      std::vector<char*> arguments;
      auto command = config.command.string();
      arguments.push_back(command.data());
      for (auto& argument : config.arguments)
        arguments.push_back(argument.data());
      arguments.push_back(nullptr);
      execvp(command.c_str(), arguments.data());
      _exit(127);
    }
    close(input_pipe[0]);
    close(output_pipe[1]);
    close(error_pipe[1]);
    input_write = input_pipe[1];
    output_read = output_pipe[0];
    error_read = error_pipe[0];
#endif
    output_reader =
        std::jthread([this](std::stop_token) { read_output(); });
    error_reader = std::jthread([this](std::stop_token) { read_errors(); });
  }

  bool alive() const noexcept {
#ifdef _WIN32
    if (!process) return false;
    DWORD code = 0;
    return GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
#else
    return pid > 0 && kill(pid, 0) == 0;
#endif
  }

  void record_error(std::string line) {
    if (line.empty()) return;
    std::lock_guard lock(state_mutex);
    stderr_lines.push_back(std::move(line));
    if (stderr_lines.size() > 100)
      stderr_lines.erase(stderr_lines.begin(), stderr_lines.begin() + 20);
  }

  void fail_pending(std::string message) {
    std::map<std::uint64_t, std::shared_ptr<Pending>> waiting;
    {
      std::lock_guard lock(state_mutex);
      waiting.swap(pending);
    }
    for (auto& [id, item] : waiting) {
      std::lock_guard item_lock(item->mutex);
      item->response = {
          {"jsonrpc", "2.0"}, {"id", id},
          {"error", {{"code", -32000}, {"message", message}}}};
      item->completed = true;
      item->ready.notify_all();
    }
  }

  void dispatch(std::string_view frame) {
    try {
      const auto message = tokmon::Json::parse(frame);
      if (!message.contains("id") || message["id"].is_null()) return;
      const auto id = message.at("id").get<std::uint64_t>();
      std::shared_ptr<Pending> item;
      {
        std::lock_guard lock(state_mutex);
        const auto found = pending.find(id);
        if (found == pending.end()) return;
        item = found->second;
        pending.erase(found);
      }
      {
        std::lock_guard item_lock(item->mutex);
        item->response = message;
        item->completed = true;
      }
      item->ready.notify_all();
    } catch (const std::exception& error) {
      record_error("invalid MCP stdout frame: " + std::string(error.what()));
    }
  }

  void read_output() {
    std::array<char, 8192> buffer{};
    std::string accumulated;
    for (;;) {
#ifdef _WIN32
      DWORD count = 0;
      if (!ReadFile(output_read, buffer.data(),
                    static_cast<DWORD>(buffer.size()), &count, nullptr) ||
          count == 0)
        break;
#else
      const auto count = ::read(output_read, buffer.data(), buffer.size());
      if (count <= 0) break;
#endif
      accumulated.append(buffer.data(), static_cast<std::size_t>(count));
      if (accumulated.size() > 8U * 1024U * 1024U) {
        record_error("MCP frame exceeded 8 MiB");
        break;
      }
      for (;;) {
        const auto newline = accumulated.find('\n');
        if (newline == std::string::npos) break;
        auto frame = accumulated.substr(0, newline);
        accumulated.erase(0, newline + 1);
        if (!frame.empty() && frame.back() == '\r') frame.pop_back();
        if (!frame.empty()) dispatch(frame);
      }
    }
    fail_pending(stopping ? "MCP server stopped" : "MCP server exited");
  }

  void read_errors() {
    std::array<char, 4096> buffer{};
    std::string accumulated;
    for (;;) {
#ifdef _WIN32
      DWORD count = 0;
      if (!ReadFile(error_read, buffer.data(),
                    static_cast<DWORD>(buffer.size()), &count, nullptr) ||
          count == 0)
        break;
#else
      const auto count = ::read(error_read, buffer.data(), buffer.size());
      if (count <= 0) break;
#endif
      accumulated.append(buffer.data(), static_cast<std::size_t>(count));
      for (;;) {
        const auto newline = accumulated.find('\n');
        if (newline == std::string::npos) break;
        record_error(accumulated.substr(0, newline));
        accumulated.erase(0, newline + 1);
      }
    }
    if (!accumulated.empty()) record_error(std::move(accumulated));
  }

  void write(tokmon::Json message) {
    const auto frame = message.dump() + "\n";
    std::lock_guard lock(write_mutex);
    std::size_t offset = 0;
    while (offset < frame.size()) {
#ifdef _WIN32
      DWORD count = 0;
      if (!WriteFile(input_write, frame.data() + offset,
                     static_cast<DWORD>(frame.size() - offset), &count,
                     nullptr))
        throw tokmon::Error("snow.mcp.write", "failed to write MCP stdin");
#else
      const auto count = ::write(input_write, frame.data() + offset,
                                 frame.size() - offset);
      if (count <= 0)
        throw tokmon::Error("snow.mcp.write", "failed to write MCP stdin");
#endif
      offset += static_cast<std::size_t>(count);
    }
  }

  tokmon::Json request(std::string method, tokmon::Json params,
                       std::stop_token stop = {}) {
    if (!alive())
      throw tokmon::Error("snow.mcp.disconnected",
                          "MCP server is not running: " + config.id);
    const auto id = next_id.fetch_add(1);
    auto item = std::make_shared<Pending>();
    {
      std::lock_guard lock(state_mutex);
      pending.emplace(id, item);
    }
    write({{"jsonrpc", "2.0"},
           {"id", id},
           {"method", std::move(method)},
           {"params", std::move(params)}});
    const auto deadline =
        std::chrono::steady_clock::now() + config.request_timeout;
    std::unique_lock lock(item->mutex);
    while (!item->completed && std::chrono::steady_clock::now() < deadline &&
           !stop.stop_requested())
      item->ready.wait_for(lock, std::chrono::milliseconds(25));
    if (!item->completed) {
      lock.unlock();
      {
        std::lock_guard state_lock(state_mutex);
        pending.erase(id);
      }
      write({{"jsonrpc", "2.0"},
             {"method", "notifications/cancelled"},
             {"params", {{"requestId", id},
                          {"reason", stop.stop_requested()
                                         ? "Snow turn cancelled"
                                         : "Snow MCP deadline exceeded"}}}});
      throw tokmon::Error(
          stop.stop_requested() ? "snow.mcp.cancelled" : "snow.mcp.timeout",
          stop.stop_requested() ? "MCP request cancelled"
                                : "MCP request timed out",
          {{"server", config.id}, {"request_id", id}});
    }
    const auto response = item->response;
    if (response.contains("error"))
      throw tokmon::Error(
          "snow.mcp.remote",
          response.at("error").value("message", "MCP request failed"),
          {{"server", config.id}, {"remote", response.at("error")}});
    return response.value("result", tokmon::Json::object());
  }

  void notify(std::string method, tokmon::Json params) {
    write({{"jsonrpc", "2.0"},
           {"method", std::move(method)},
           {"params", std::move(params)}});
  }

  void stop() noexcept {
    if (stopping.exchange(true)) return;
    try {
      if (alive()) notify("notifications/cancelled", {{"reason", "shutdown"}});
    } catch (...) {
    }
#ifdef _WIN32
    if (input_write) {
      CloseHandle(input_write);
      input_write = nullptr;
    }
    if (process && WaitForSingleObject(process, 500) == WAIT_TIMEOUT) {
      if (job) TerminateJobObject(job, 0);
      else TerminateProcess(process, 0);
    }
#else
    if (input_write >= 0) {
      close(input_write);
      input_write = -1;
    }
    if (pid > 0) {
      int status = 0;
      if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
      }
    }
#endif
    if (output_reader.joinable() &&
        output_reader.get_id() != std::this_thread::get_id())
      output_reader.join();
    if (error_reader.joinable() &&
        error_reader.get_id() != std::this_thread::get_id())
      error_reader.join();
#ifdef _WIN32
    if (output_read) CloseHandle(output_read);
    if (error_read) CloseHandle(error_read);
    if (process) CloseHandle(process);
    if (job) CloseHandle(job);
    output_read = error_read = process = job = nullptr;
    pid = 0;
#else
    if (output_read >= 0) close(output_read);
    if (error_read >= 0) close(error_read);
    output_read = error_read = -1;
    pid = -1;
#endif
    fail_pending("MCP server stopped");
  }

  tokmon::Json diagnostics() const {
    std::lock_guard lock(state_mutex);
    return {{"server", config.id},
            {"alive", alive()},
            {"pid", static_cast<std::uint64_t>(pid)},
            {"pending", pending.size()},
            {"stderr", stderr_lines}};
  }
};

namespace {

class McpTool final : public Tool {
public:
  McpTool(std::shared_ptr<McpToolProvider::Impl> client,
          std::string server, tokmon::Json remote)
      : client_(std::move(client)),
        remote_name_(remote.at("name").get<std::string>()) {
    definition_.name = "mcp__" + safe_component(std::move(server)) + "__" +
                       safe_component(remote_name_);
    definition_.description = remote.value("description", "MCP tool") +
                              " (provided by an isolated MCP server)";
    definition_.input_schema =
        remote.value("inputSchema", tokmon::Json{{"type", "object"}});
    if (!definition_.input_schema.is_object())
      definition_.input_schema = {{"type", "object"}};
    // Remote annotations are untrusted hints. DefaultPolicy therefore asks
    // before every MCP operation, including remotely-annotated read-only calls.
    definition_.read_only = false;
    definition_.parallel_safe = false;
  }

  const ToolDefinition& definition() const override { return definition_; }

  ToolResult execute(const tokmon::Json& arguments,
                     std::stop_token stop) override {
    try {
      const auto result = client_->request(
          "tools/call", {{"name", remote_name_}, {"arguments", arguments}},
          stop);
      std::string content;
      for (const auto& item :
           result.value("content", tokmon::Json::array())) {
        if (item.value("type", "") == "text") {
          if (!content.empty()) content += '\n';
          content += item.value("text", "");
        } else {
          if (!content.empty()) content += '\n';
          content += item.dump();
        }
      }
      const auto failed = result.value("isError", false);
      return {!failed, failed ? "error" : "ok", std::move(content),
              {{"mcp_server", client_->config.id},
               {"mcp_tool", remote_name_},
               {"structured_content",
                result.value("structuredContent", tokmon::Json(nullptr))}}};
    } catch (const tokmon::Error& error) {
      return {false,
              error.code() == "snow.mcp.cancelled" ? "aborted" : "error",
              error.what(),
              {{"mcp_server", client_->config.id},
               {"mcp_tool", remote_name_},
               {"error_code", error.code()}}};
    }
  }

private:
  std::shared_ptr<McpToolProvider::Impl> client_;
  std::string remote_name_;
  ToolDefinition definition_;
};

} // namespace

McpToolProvider::McpToolProvider(McpServerConfig config)
    : impl_(std::make_shared<Impl>(std::move(config))) {
  impl_->start();
  const auto initialized = impl_->request(
      "initialize",
      {{"protocolVersion", "2025-06-18"},
       {"capabilities", tokmon::Json::object()},
       {"clientInfo", {{"name", "snow"}, {"version", "1.0.0"}}}});
  if (!initialized.contains("serverInfo"))
    throw tokmon::Error("snow.mcp.initialize",
                        "MCP initialize response has no serverInfo",
                        {{"server", impl_->config.id}});
  impl_->notify("notifications/initialized", tokmon::Json::object());

  std::optional<std::string> cursor;
  do {
    auto params = tokmon::Json::object();
    if (cursor) params["cursor"] = *cursor;
    const auto listed = impl_->request("tools/list", std::move(params));
    for (const auto& remote :
         listed.value("tools", tokmon::Json::array()))
      tools_.push_back(
          std::make_shared<McpTool>(impl_, impl_->config.id, remote));
    cursor.reset();
    if (listed.contains("nextCursor") && listed["nextCursor"].is_string())
      cursor = listed["nextCursor"].get<std::string>();
  } while (cursor);
}

McpToolProvider::~McpToolProvider() = default;
const std::string& McpToolProvider::id() const noexcept {
  return impl_->config.id;
}
const std::vector<std::shared_ptr<Tool>>& McpToolProvider::tools() const {
  return tools_;
}
tokmon::Json McpToolProvider::diagnostics() const {
  return impl_->diagnostics();
}
void McpToolProvider::stop() noexcept { impl_->stop(); }

std::vector<McpServerConfig> load_mcp_config(
    const std::filesystem::path& mcp_json,
    const std::filesystem::path& workspace) {
  if (!std::filesystem::exists(mcp_json)) return {};
  const auto document = load_json_config(mcp_json);
  if (document.value("schema", "") != "org.tokmon.snow.mcp/v1" ||
      !document.contains("servers") || !document.at("servers").is_array())
    throw tokmon::Error("snow.mcp.schema", "invalid MCP configuration schema");
  std::vector<McpServerConfig> result;
  for (const auto& server : document.at("servers")) {
    if (!server.is_object())
      throw tokmon::Error("snow.mcp.config", "MCP server must be an object");
    // Disabled providers are inert configuration. In particular, do not
    // resolve their secret environment references until they are enabled.
    if (!server.value("enabled", true)) continue;
    auto config = McpServerConfig::parse(server, workspace);
    result.push_back(std::move(config));
  }
  std::ranges::sort(result, {}, &McpServerConfig::id);
  const auto duplicate = std::ranges::adjacent_find(
      result, {}, &McpServerConfig::id);
  if (duplicate != result.end())
    throw tokmon::Error("snow.mcp.duplicate",
                        "duplicate MCP server id: " + duplicate->id);
  return result;
}

} // namespace snow
