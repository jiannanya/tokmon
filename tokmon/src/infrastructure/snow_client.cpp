#include <tokmon/snow_client.hpp>

#include <tokmon/common/files.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
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

namespace tokmon::desktop {
namespace {

std::string request_key(const tokmon::Json& id) {
  return id.dump();
}

#ifdef _WIN32
std::wstring widen_utf8(std::string_view value) {
  if (value.empty()) return {};
  const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        value.data(),
                                        static_cast<int>(value.size()),
                                        nullptr, 0);
  if (size <= 0) return std::wstring(value.begin(), value.end());
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size);
  return result;
}

std::string narrow_utf8(std::wstring_view value) {
  if (value.empty()) return {};
  const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                        value.data(),
                                        static_cast<int>(value.size()),
                                        nullptr, 0, nullptr, nullptr);
  if (size <= 0) return "Windows error";
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

std::wstring quote_windows_argument(const std::wstring& value) {
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

std::string windows_message(DWORD code) {
  wchar_t* buffer = nullptr;
  const auto size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::string value = size ? narrow_utf8(std::wstring_view(buffer, size))
                           : "Windows error " + std::to_string(code);
  if (buffer) LocalFree(buffer);
  return value;
}
#endif

} // namespace

class SnowProcessClient::Impl final {
public:
  explicit Impl(SnowProcessOptions value) : options(std::move(value)) {}
  ~Impl() { stop(); }

  struct Pending {
    std::mutex mutex;
    std::condition_variable ready;
    bool completed{false};
    tokmon::Json result;
    std::optional<tokmon::Error> error;
  };

  SnowProcessOptions options;
  mutable std::mutex state_mutex;
  mutable std::mutex write_mutex;
  std::map<std::string, std::shared_ptr<Pending>, std::less<>> pending;
  NotificationHandler notifications;
  CrashHandler crash;
  std::deque<std::string> stderr_lines;
  std::atomic_uint64_t next_id{1};
  std::atomic_bool stopping{false};
  std::jthread reader;
  std::jthread stderr_reader;

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

  void fail_pending(std::string message) {
    std::map<std::string, std::shared_ptr<Pending>, std::less<>> waiting;
    {
      std::lock_guard lock(state_mutex);
      waiting.swap(pending);
    }
    for (auto& [_, item] : waiting) {
      {
        std::lock_guard lock(item->mutex);
        item->error.emplace("tokmon.snow.disconnected", message);
        item->completed = true;
      }
      item->ready.notify_all();
    }
  }

  void record_diagnostic(std::string line) {
    if (line.empty()) return;
    std::lock_guard lock(state_mutex);
    stderr_lines.push_back(std::move(line));
    while (stderr_lines.size() > 200) stderr_lines.pop_front();
  }

  std::vector<std::string> diagnostic_snapshot() const {
    std::lock_guard lock(state_mutex);
    return {stderr_lines.begin(), stderr_lines.end()};
  }

  void dispatch(std::string_view line) {
    tokmon::Json message;
    try {
      message = tokmon::Json::parse(line);
    } catch (const std::exception& error) {
      record_diagnostic("invalid Snow stdout frame: " +
                        std::string(error.what()));
      return;
    }
    if (message.contains("id") && !message["id"].is_null()) {
      std::shared_ptr<Pending> item;
      {
        std::lock_guard lock(state_mutex);
        const auto found = pending.find(request_key(message["id"]));
        if (found != pending.end()) {
          item = found->second;
          pending.erase(found);
        }
      }
      if (!item) return;
      {
        std::lock_guard lock(item->mutex);
        if (message.contains("error")) {
          const auto& value = message["error"];
          item->error.emplace(value.value("code", "snow.protocol"),
                              value.value("message", "Snow request failed"),
                              value.value("data", tokmon::Json::object()));
        } else {
          item->result = message.value("result", tokmon::Json(nullptr));
        }
        item->completed = true;
      }
      item->ready.notify_all();
      return;
    }
    NotificationHandler handler;
    {
      std::lock_guard lock(state_mutex);
      handler = notifications;
    }
    if (handler) handler(message);
  }

  void read_stdout(std::stop_token) {
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
      for (;;) {
        const auto newline = accumulated.find('\n');
        if (newline == std::string::npos) break;
        auto line = accumulated.substr(0, newline);
        accumulated.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) dispatch(line);
      }
    }
    if (!accumulated.empty()) dispatch(accumulated);
    const bool expected = stopping.load();
    std::uint32_t code = 0;
#ifdef _WIN32
    if (process) {
      DWORD native_code = 0;
      if (GetExitCodeProcess(process, &native_code) &&
          native_code != STILL_ACTIVE)
        code = native_code;
    }
#endif
    fail_pending(expected ? "Snow stopped" : "Snow process exited");
    CrashHandler handler;
    {
      std::lock_guard lock(state_mutex);
      handler = crash;
    }
    if (handler) handler({code, expected, diagnostic_snapshot()});
  }

  void read_stderr(std::stop_token) {
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
        auto line = accumulated.substr(0, newline);
        accumulated.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        record_diagnostic(std::move(line));
      }
    }
    if (!accumulated.empty()) record_diagnostic(std::move(accumulated));
  }

  void start() {
    if (alive()) return;
    if (options.executable.empty()) {
      throw tokmon::Error("tokmon.snow.executable",
                          "Snow executable path is empty");
    }
    stopping = false;
    std::filesystem::create_directories(options.data_root);
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
      const auto error = GetLastError();
      cleanup();
      throw tokmon::Error("tokmon.snow.pipe", windows_message(error));
    }
    SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(error_read, HANDLE_FLAG_INHERIT, 0);

    const auto executable = std::filesystem::absolute(options.executable);
    std::wstring command = quote_windows_argument(executable.wstring()) +
                           L" serve --workspace " +
                           quote_windows_argument(options.workspace.wstring()) +
                           L" --config-dir-name " +
                           quote_windows_argument(widen_utf8(
                               options.config_dir_name)) +
                           L" --data-root " +
                           quote_windows_argument(options.data_root.wstring());
    if (options.raw_trace) command += L" --raw-trace";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input_read;
    startup.hStdOutput = output_write;
    startup.hStdError = error_write;
    PROCESS_INFORMATION info{};
    const auto created = CreateProcessW(
        executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
        options.workspace.c_str(), &startup, &info);
    cleanup();
    if (!created) {
      const auto error = GetLastError();
      CloseHandle(input_write);
      CloseHandle(output_read);
      CloseHandle(error_read);
      input_write = output_read = error_read = nullptr;
      throw tokmon::Error("tokmon.snow.launch", windows_message(error),
                          {{"executable", executable.generic_string()}});
    }
    CloseHandle(info.hThread);
    process = info.hProcess;
    pid = info.dwProcessId;
    job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
      TerminateProcess(process, 1);
      throw tokmon::Error("tokmon.snow.job",
                          windows_message(GetLastError()));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                            sizeof(limits));
    if (!AssignProcessToJobObject(job, process)) {
      const auto error = GetLastError();
      TerminateProcess(process, 1);
      CloseHandle(job);
      job = nullptr;
      throw tokmon::Error("tokmon.snow.job", windows_message(error));
    }
#else
    int in_pipe[2]{};
    int out_pipe[2]{};
    int err_pipe[2]{};
    if (pipe(in_pipe) || pipe(out_pipe) || pipe(err_pipe)) {
      throw tokmon::Error("tokmon.snow.pipe", "failed to create stdio pipes");
    }
    pid = fork();
    if (pid == 0) {
      dup2(in_pipe[0], STDIN_FILENO);
      dup2(out_pipe[1], STDOUT_FILENO);
      dup2(err_pipe[1], STDERR_FILENO);
      execl(options.executable.c_str(), options.executable.c_str(), "serve",
            "--workspace", options.workspace.c_str(), "--config-dir-name",
            options.config_dir_name.c_str(), "--data-root",
            options.data_root.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    input_write = in_pipe[1];
    output_read = out_pipe[0];
    error_read = err_pipe[0];
#endif
    reader = std::jthread([this](std::stop_token token) {
      read_stdout(token);
    });
    stderr_reader = std::jthread([this](std::stop_token token) {
      read_stderr(token);
    });
  }

  void stop() {
    if (stopping.exchange(true)) return;
#ifdef _WIN32
    if (input_write) {
      CloseHandle(input_write);
      input_write = nullptr;
    }
    if (process) {
      if (WaitForSingleObject(process, 1500) == WAIT_TIMEOUT) {
        if (job) TerminateJobObject(job, 0);
        WaitForSingleObject(process, 3000);
      }
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
    if (reader.joinable() && reader.get_id() != std::this_thread::get_id())
      reader.join();
    if (stderr_reader.joinable() &&
        stderr_reader.get_id() != std::this_thread::get_id())
      stderr_reader.join();
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
    fail_pending("Snow stopped");
  }

  bool alive() const {
#ifdef _WIN32
    if (!process) return false;
    DWORD code = 0;
    return GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
#else
    if (pid <= 0) return false;
    return kill(pid, 0) == 0;
#endif
  }

  std::uint32_t process_id() const {
#ifdef _WIN32
    return pid;
#else
    return pid > 0 ? static_cast<std::uint32_t>(pid) : 0;
#endif
  }

  void write_frame(std::string_view frame) {
    std::lock_guard lock(write_mutex);
    std::size_t offset = 0;
    while (offset < frame.size()) {
#ifdef _WIN32
      DWORD count = 0;
      if (!WriteFile(input_write, frame.data() + offset,
                     static_cast<DWORD>(frame.size() - offset), &count,
                     nullptr)) {
        throw tokmon::Error("tokmon.snow.write",
                            windows_message(GetLastError()));
      }
#else
      const auto count =
          ::write(input_write, frame.data() + offset, frame.size() - offset);
      if (count <= 0)
        throw tokmon::Error("tokmon.snow.write", "failed to write Snow stdin");
#endif
      offset += static_cast<std::size_t>(count);
    }
  }

  tokmon::Json request(std::string_view method, tokmon::Json params,
                       std::chrono::milliseconds timeout) {
    if (!alive())
      throw tokmon::Error("tokmon.snow.disconnected",
                          "Snow process is not running");
    const auto id = next_id.fetch_add(1);
    auto item = std::make_shared<Pending>();
    {
      std::lock_guard lock(state_mutex);
      pending.emplace(std::to_string(id), item);
    }
    const auto frame = tokmon::Json{{"jsonrpc", "2.0"},
                                    {"id", id},
                                    {"method", method},
                                    {"params", std::move(params)}}
                           .dump() +
                       "\n";
    try {
      write_frame(frame);
    } catch (...) {
      std::lock_guard lock(state_mutex);
      pending.erase(std::to_string(id));
      throw;
    }
    if (timeout == std::chrono::milliseconds::zero())
      timeout = options.request_timeout;
    std::unique_lock lock(item->mutex);
    if (!item->ready.wait_for(lock, timeout,
                              [&] { return item->completed; })) {
      lock.unlock();
      {
        std::lock_guard state_lock(state_mutex);
        pending.erase(std::to_string(id));
      }
      throw tokmon::Error("tokmon.snow.timeout",
                          "Snow request timed out",
                          {{"method", method},
                           {"timeout_ms", timeout.count()}});
    }
    if (item->error) throw *item->error;
    return item->result;
  }
};

SnowProcessClient::SnowProcessClient(SnowProcessOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
SnowProcessClient::~SnowProcessClient() = default;

void SnowProcessClient::start() { impl_->start(); }
void SnowProcessClient::stop() { impl_->stop(); }
bool SnowProcessClient::alive() const { return impl_->alive(); }
std::uint32_t SnowProcessClient::process_id() const {
  return impl_->process_id();
}

tokmon::Json SnowProcessClient::request(std::string_view method,
                                        tokmon::Json params,
                                        std::chrono::milliseconds timeout) {
  return impl_->request(method, std::move(params), timeout);
}

tokmon::Json SnowProcessClient::initialize() {
  return request("initialize",
                 {{"protocol_min", 1},
                  {"protocol_max", 1},
                  {"client", {{"name", "tokmon"}, {"version", "1.0.0"}}}});
}

void SnowProcessClient::set_notification_handler(
    NotificationHandler handler) {
  std::lock_guard lock(impl_->state_mutex);
  impl_->notifications = std::move(handler);
}

void SnowProcessClient::set_crash_handler(CrashHandler handler) {
  std::lock_guard lock(impl_->state_mutex);
  impl_->crash = std::move(handler);
}

std::vector<std::string> SnowProcessClient::diagnostics() const {
  return impl_->diagnostic_snapshot();
}

std::filesystem::path SnowProcessClient::sibling_snow_executable() {
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const auto size = GetModuleFileNameW(nullptr, buffer.data(),
                                      static_cast<DWORD>(buffer.size()));
  if (size == 0 || size == buffer.size()) return "snow.exe";
  buffer.resize(size);
  return std::filesystem::path(buffer).parent_path() / "snow.exe";
#else
  std::array<char, 4096> buffer{};
  const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (size <= 0) return "snow";
  return std::filesystem::path(std::string(buffer.data(), size)).parent_path() /
         "snow";
#endif
}

} // namespace tokmon::desktop
