#include <tokmon/app.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <snow/config.hpp>
#include <sstream>
#include <tokmon/common/digest.hpp>
#include <tokmon/common/files.hpp>

namespace tokmon::desktop {
namespace {

std::filesystem::path
resolve_config_path(const std::filesystem::path &workspace,
                    const std::filesystem::path &default_value,
                    const tokmon::Json &document, std::string_view key) {
  const auto configured = document.value(std::string(key), "");
  if (configured.empty())
    return default_value;
  const std::filesystem::path value(configured);
  return value.is_absolute() ? value : workspace / value;
}

std::vector<std::byte> decode_base64(std::string_view input) {
  std::array<int, 256> values{};
  values.fill(-1);
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (std::size_t index = 0; index < alphabet.size(); ++index)
    values[static_cast<unsigned char>(alphabet[index])] =
        static_cast<int>(index);
  std::vector<std::byte> result;
  unsigned accumulator = 0;
  int bits = -8;
  for (const auto character : input) {
    if (character == '=')
      break;
    const auto value = values[static_cast<unsigned char>(character)];
    if (value < 0)
      throw tokmon::Error("tokmon.artifact.base64",
                          "Snow returned invalid artifact base64");
    accumulator = (accumulator << 6U) | static_cast<unsigned>(value);
    bits += 6;
    if (bits >= 0) {
      result.push_back(static_cast<std::byte>((accumulator >> bits) & 0xffU));
      bits -= 8;
    }
  }
  return result;
}

std::string artifact_preview(std::span<const std::byte> bytes,
                             std::string_view media_type) {
  if (media_type.starts_with("text/") ||
      media_type.find("json") != std::string_view::npos ||
      media_type.find("xml") != std::string_view::npos) {
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
  }
  return "Binary artifact (" + std::to_string(bytes.size()) + " bytes, " +
         std::string(media_type) + ")";
}

std::string *editable_setting(DesktopSettings &settings,
                              std::string_view field) {
  if (field == "provider_id")
    return &settings.provider_id;
  if (field == "provider_name")
    return &settings.provider_name;
  if (field == "endpoint")
    return &settings.endpoint;
  if (field == "api_key_env")
    return &settings.api_key_env;
  if (field == "model")
    return &settings.model;
  if (field == "request_timeout_ms")
    return &settings.request_timeout_ms;
  if (field == "max_steps")
    return &settings.max_steps;
  return nullptr;
}

std::size_t setting_number(std::string_view value, std::size_t fallback,
                           std::size_t minimum, std::size_t maximum) {
  std::size_t result{};
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
    return fallback;
  return std::clamp(result, minimum, maximum);
}

} // namespace

AppConfig load_app_config(const std::filesystem::path &workspace,
                          std::string config_dir_name) {
  AppConfig result;
  result.workspace = std::filesystem::weakly_canonical(workspace);
  result.config_dir_name = std::move(config_dir_name);
  const auto layout =
      snow::ConfigLayout::resolve(result.workspace, result.config_dir_name);
  const auto path = layout.config_root / "tokmon.json";
  const auto document = snow::load_json_config(path, tokmon::Json::object());
  if (!document.empty() &&
      document.value("schema", "") != "org.tokmon.desktop.config/v1")
    throw tokmon::Error("tokmon.config.schema", "invalid tokmon.json schema");
  const auto snow_config = document.value("snow", tokmon::Json::object());
  const auto ui_config = document.value("ui", tokmon::Json::object());
  const auto agent_config = document.value("agent", tokmon::Json::object());
  const auto restart = snow_config.value("restart", tokmon::Json::object());

  result.data_root = resolve_config_path(
      result.workspace, layout.config_root / "data", document, "data_root");
  result.snow_executable = resolve_config_path(
      result.workspace, SnowProcessClient::sibling_snow_executable(),
      snow_config, "executable");
  result.embedded_snow = snow_config.value("mode", "process") == "embedded";
  result.raw_trace = snow_config.value("raw_trace", false);
  result.restart_enabled = restart.value("enabled", true);
  result.restart_max_attempts = restart.value("max_attempts", 5U);
  result.restart_base_delay =
      std::chrono::milliseconds(restart.value("base_delay_ms", 250U));
  result.poll_interval =
      std::chrono::milliseconds(ui_config.value("poll_interval_ms", 25U));
  result.ui_scale =
      std::clamp(ui_config.value("scale", 1.25F), 0.75F, 2.0F);
  result.request_timeout = std::chrono::milliseconds(
      snow_config.value("request_timeout_ms", 300000U));
  result.model = document.value("model", "");
  result.max_steps = agent_config.value("max_steps", 32U);
  result.max_steps = std::clamp<std::size_t>(result.max_steps, 1, 1024);
  if (result.restart_max_attempts == 0)
    result.restart_max_attempts = 1;
  if (result.poll_interval < std::chrono::milliseconds(5))
    result.poll_interval = std::chrono::milliseconds(5);
  return result;
}

App::App(AppConfig config)
    : config_(std::move(config)), workbench_(config_.workspace),
      white_(ui_runtime_), approvals_(std::make_shared<ApprovalCoordinator>()),
      projection_(std::make_shared<Projection>()) {
  settings_ = load_desktop_settings(config_.workspace, config_.config_dir_name);
  if (config_.model.empty())
    config_.model = settings_.model;
  config_.max_steps =
      setting_number(settings_.max_steps, config_.max_steps, 1, 1024);
  window_ = white_.service().create_window({.title = "Tokmon · Arche Agent OS",
                                            .width = 1500,
                                            .height = 900,
                                            .resizable = true,
                                            .borderless = true,
                                            .ui_scale = config_.ui_scale,
                                            .opaque_draw = true});
  white::RasterSurface brand_icon(32, 32);
  brand_icon.clear({0, 0, 0, 0});
  constexpr white::Color brand_ink{92, 95, 102, 255};
  brand_icon.fill_circle(11, 8, 2.5F, brand_ink);
  brand_icon.fill_circle(20, 16, 2.5F, brand_ink);
  brand_icon.fill_circle(11, 24, 2.5F, brand_ink);
  brand_icon.line(11, 10.5F, 11, 21.5F, brand_ink, 1.8F);
  brand_icon.line(13.5F, 16, 17.5F, 16, brand_ink, 1.8F);
  (void)window_->set_icon(brand_icon);
  window_->set_builtin_chrome(false);
  window_->set_draw_callback(
      [this](white::RasterSurface &surface) { draw(surface); });
  window_->set_submit_callback([this](std::string message) {
    handle_editor_submit(std::move(message));
  });
  window_->set_event_callback([this](white::UiEvent event) {
    return handle_workbench_event(event);
  });
  approvals_->set_changed([this] {
    if (window_)
      window_->invalidate();
  });

  const auto state_path =
      config_.workspace / config_.config_dir_name / "state.json";
  if (std::filesystem::exists(state_path)) {
    const auto state =
        snow::load_json_config(state_path, tokmon::Json::object());
    session_ = tokmon::SessionId(state.value("session_id", ""));
  }

  if (config_.embedded_snow) {
    snow::BootstrapConfig snow_config;
    snow_config.workspace = config_.workspace;
    snow_config.config_dir_name = config_.config_dir_name;
    snow_config.data_root = config_.data_root;
    snow_config.raw_trace_enabled = config_.raw_trace;
    embedded_snow_ = std::make_unique<snow::Assembly>(
        snow_config, create_model(), approvals_);
    if (!session_.empty()) {
      try {
        projection_->replay(embedded_snow_->agent().events(session_));
      } catch (const tokmon::Error &error) {
        if (error.code() != "snow.session.missing")
          throw;
        session_ = {};
      }
    }
    if (session_.empty()) {
      session_ = embedded_snow_->agent().create_session(
          {{"origin", "tokmon-desktop"}, {"mode", "embedded"}});
      persist_session();
    }
    auto journal =
        embedded_snow_->arche_runtime()
            .root_context()
            ->require<snow::TrajectoryJournal>("snow.trajectory", "^1.0");
    event_connection_ = journal->committed().connect(
        [this](const snow::TrajectoryEvent &event) {
          if (event.session_id == session_) {
            projection_->apply(event);
            if (window_)
              window_->invalidate();
          }
        });
    update_status("Snow embedded runtime ready");
  } else {
    snow_process_ = std::make_shared<SnowProcessClient>(
        SnowProcessOptions{.executable = config_.snow_executable,
                           .workspace = config_.workspace,
                           .data_root = config_.data_root,
                           .config_dir_name = config_.config_dir_name,
                           .raw_trace = config_.raw_trace,
                           .request_timeout = config_.request_timeout});
    snow_process_->set_notification_handler(
        [this](const tokmon::Json &value) { handle_notification(value); });
    snow_process_->set_crash_handler([this](const SnowProcessExit &exit) {
      if (exit.expected)
        return;
      {
        std::lock_guard lock(state_mutex_);
        if (shutting_down_)
          return;
        restart_requested_ = config_.restart_enabled;
        turn_active_ = false;
      }
      update_status("Snow exited with code " + std::to_string(exit.code));
      supervisor_ready_.notify_all();
    });
    connect_child(true);
    supervisor_ =
        std::jthread([this](std::stop_token stop) { supervise_child(stop); });
    poller_ = std::jthread([this](std::stop_token stop) { poll_child(stop); });
  }
  refresh_sessions();
  product_ = std::make_unique<ProductAssembly>(ui_runtime_, projection_,
                                               approvals_, snow_process_);
}

App::~App() {
  {
    std::lock_guard lock(state_mutex_);
    shutting_down_ = true;
  }
  approvals_->cancel();
  supervisor_ready_.notify_all();
  if (poller_.joinable()) {
    poller_.request_stop();
    poller_.join();
  }
  if (supervisor_.joinable()) {
    supervisor_.request_stop();
    supervisor_ready_.notify_all();
    supervisor_.join();
  }
  if (active_turn_.joinable()) {
    if (snow_process_ && snow_process_->alive() && !session_.empty()) {
      try {
        (void)snow_process_->request("turn.cancel",
                                     {{"session_id", session_.str()}},
                                     std::chrono::seconds(2));
      } catch (...) {
      }
    }
    active_turn_.request_stop();
    active_turn_.join();
  }
  if (snow_process_)
    snow_process_->stop();
}

int App::run() { return window_->run(); }

void App::capture(const std::filesystem::path &path) {
  window_->save_screenshot(path);
}

int App::smoke() {
  submit("/diagnostics");
  window_->render_once();
  return projection_->snapshot().empty() ? 1 : 0;
}

void App::submit(std::string message) {
  if (const auto pending = approvals_->pending()) {
    if (message == "/approve" || message == "approve" || message == "y") {
      approvals_->resolve(true);
      return;
    }
    if (message == "/deny" || message == "deny" || message == "n") {
      approvals_->resolve(false);
      return;
    }
    update_status("Approval pending: type /approve or /deny");
    return;
  }

  if (message == "/cancel") {
    bool active = false;
    {
      std::lock_guard lock(state_mutex_);
      active = turn_active_;
    }
    if (!active) {
      update_status("No active turn to cancel");
      return;
    }
    if (snow_process_) {
      try {
        const auto result = snow_process_->request(
            "turn.cancel", {{"session_id", session_.str()}},
            std::chrono::seconds(2));
        update_status(result.value("accepted", false)
                          ? "Cancellation requested"
                          : "Turn already finished");
      } catch (const std::exception &error) {
        update_status("Cancel failed: " + std::string(error.what()));
      }
    } else if (active_turn_.joinable()) {
      active_turn_.request_stop();
      update_status("Cancellation requested");
    }
    return;
  }

  if (message.starts_with("/steer ")) {
    bool active = false;
    {
      std::lock_guard lock(state_mutex_);
      active = turn_active_;
    }
    if (!active) {
      update_status("Steer requires an active turn");
      return;
    }
    try {
      const auto direction = message.substr(7);
      if (snow_process_) {
        (void)snow_process_->request(
            "turn.steer",
            {{"session_id", session_.str()}, {"message", direction}},
            std::chrono::seconds(5));
      } else {
        (void)embedded_snow_->agent().steer(session_, direction);
      }
      update_status("Steering message committed");
    } catch (const std::exception &error) {
      update_status("Steer failed: " + std::string(error.what()));
    }
    return;
  }

  if (message == "/fork") {
    try {
      const auto seed = projection_->cursor();
      if (snow_process_) {
        const auto forked = snow_process_->request(
            "session.fork",
            {{"session_id", session_.str()},
             {"seed_seq", seed},
             {"metadata", {{"origin", "tokmon-desktop"}}}},
            std::chrono::seconds(10));
        session_ =
            tokmon::SessionId(forked.at("session_id").get<std::string>());
      } else {
        session_ = embedded_snow_->agent().fork_session(
            session_, {{"origin", "tokmon-desktop"}}, seed);
      }
      projection_->begin_fork();
      persist_session();
      refresh_sessions();
      update_status("Forked session " + session_.str());
    } catch (const std::exception &error) {
      update_status("Fork failed: " + std::string(error.what()));
    }
    return;
  }

  if (message == "/restart" && snow_process_) {
    {
      std::lock_guard lock(state_mutex_);
      restart_requested_ = true;
    }
    supervisor_ready_.notify_all();
    update_status("Snow restart requested");
    return;
  }

  if (message == "/help") {
    projection_->append_local(
        ItemKind::status, "Tokmon commands",
        "/new\n/cancel\n/steer <message>\n/fork\n/restart\n/diagnostics\n"
        "/inspect\n/replay [R0|R1|R2]\n"
        "/artifact <sha256> [media-type]");
    window_->invalidate();
    return;
  }

  if (message == "/new") {
    {
      std::lock_guard lock(state_mutex_);
      if (turn_active_) {
        status_ = "Finish or cancel the active turn before creating a session";
        window_->set_status(status_);
        window_->invalidate();
        return;
      }
    }
    try {
      if (snow_process_) {
        const auto created = snow_process_->request(
            "session.create",
            {{"metadata",
              {{"origin", "tokmon-desktop"}, {"transport", "child-process"}}}},
            std::chrono::seconds(10));
        session_ =
            tokmon::SessionId(created.at("session_id").get<std::string>());
      } else {
        session_ = embedded_snow_->agent().create_session(
            {{"origin", "tokmon-desktop"}, {"mode", "embedded"}});
      }
      projection_->clear();
      persist_session();
      refresh_sessions();
      update_status("Created session " + session_.str());
    } catch (const std::exception &error) {
      update_status("New session failed: " + std::string(error.what()));
    }
    return;
  }

  if (message == "/diagnostics" || message == "/inspect" ||
      message == "/replay" || message.starts_with("/replay ")) {
    try {
      tokmon::Json value;
      std::string title;
      if (message == "/diagnostics") {
        title = "Arche / Snow diagnostics";
        value = snow_process_
                    ? snow_process_->request("diagnostics.inspect", {},
                                             std::chrono::seconds(5))
                    : embedded_snow_->arche_runtime().inspect();
      } else if (message == "/inspect") {
        title = "Composition inspector";
        value =
            snow_process_
                ? snow_process_->request("composition.inspect", {},
                                         std::chrono::seconds(5))
                : tokmon::Json{
                      {"epoch", embedded_snow_->arche_runtime().epoch()},
                      {"runtime", embedded_snow_->arche_runtime().inspect()}};
      } else {
        title = "Trajectory replay";
        const auto level = message.size() > 8 ? message.substr(8) : "R2";
        if (level != "R0" && level != "R1" && level != "R2")
          throw tokmon::Error("tokmon.replay.level",
                              "replay level must be R0, R1, or R2");
        if (snow_process_)
          value = snow_process_->request(
              "session.replay",
              {{"session_id", session_.str()}, {"level", level}},
              std::chrono::seconds(10));
        else {
          const auto replay = embedded_snow_->agent().replay(
              session_,
              level == "R0"
                  ? snow::ReplayLevel::transcript
                  : (level == "R1" ? snow::ReplayLevel::request_reconstruction
                                   : snow::ReplayLevel::control));
          value = {{"level", snow::to_string(replay.level)},
                   {"deterministic", replay.deterministic},
                   {"degradations", replay.degradations},
                   {"transcript", replay.transcript},
                   {"requests", replay.requests},
                   {"control", replay.control},
                   {"final_state", replay.final_state}};
        }
      }
      projection_->append_local(ItemKind::diagnostic, std::move(title),
                                value.dump(2), "inspected", value);
      window_->invalidate();
    } catch (const std::exception &error) {
      update_status("Inspection failed: " + std::string(error.what()));
    }
    return;
  }

  if (message.starts_with("/artifact ")) {
    try {
      std::istringstream arguments(message.substr(10));
      std::string sha256;
      std::string media_type = "text/plain; charset=utf-8";
      arguments >> sha256;
      if (sha256.starts_with("sha256:"))
        sha256.erase(0, 7);
      if (arguments >> std::ws && !arguments.eof())
        std::getline(arguments, media_type);
      if (sha256.size() != 64)
        throw tokmon::Error("tokmon.artifact.sha256",
                            "artifact SHA-256 must contain 64 hex digits");
      std::vector<std::byte> bytes;
      if (snow_process_) {
        std::uint64_t offset = 0;
        bool complete = false;
        while (!complete && bytes.size() < 8U * 1024U * 1024U) {
          const auto chunk = snow_process_->request("artifact.read",
                                                    {{"sha256", sha256},
                                                     {"media_type", media_type},
                                                     {"offset", offset},
                                                     {"limit", 256U * 1024U}},
                                                    std::chrono::seconds(10));
          const auto decoded =
              decode_base64(chunk.at("data_base64").get<std::string>());
          bytes.insert(bytes.end(), decoded.begin(), decoded.end());
          offset = chunk.at("next_offset").get<std::uint64_t>();
          complete = chunk.at("eof").get<bool>();
        }
        if (!complete)
          throw tokmon::Error("tokmon.artifact.limit",
                              "artifact preview exceeds 8 MiB");
      } else {
        auto artifacts =
            embedded_snow_->arche_runtime()
                .root_context()
                ->require<snow::ArtifactStore>("snow.artifacts", "^1.0");
        bytes = artifacts->read({.id = "sha256:" + sha256,
                                 .sha256 = sha256,
                                 .media_type = media_type});
      }
      projection_->append_local(ItemKind::artifact,
                                "Artifact / " + sha256.substr(0, 12),
                                artifact_preview(bytes, media_type), "loaded",
                                {{"sha256", sha256},
                                 {"media_type", media_type},
                                 {"bytes", bytes.size()}});
      window_->invalidate();
    } catch (const std::exception &error) {
      update_status("Artifact read failed: " + std::string(error.what()));
    }
    return;
  }

  tokmon::Json turn_attachments = tokmon::Json::array();
  {
    std::lock_guard lock(state_mutex_);
    if (turn_active_) {
      status_ = "A turn is already running";
      window_->set_status(status_);
      return;
    }
    if (snow_process_ && !snow_process_->alive()) {
      status_ = "Snow is disconnected; use /restart";
      window_->set_status(status_);
      return;
    }
    turn_active_ = true;
    for (const auto &attachment : attachments_) {
      turn_attachments.push_back({{"name", attachment.name},
                                  {"path", attachment.path.generic_string()},
                                  {"sha256", attachment.sha256},
                                  {"bytes", attachment.content.size()},
                                  {"content", attachment.content}});
    }
    attachments_.clear();
    message_draft_.clear();
  }
  update_status("Snow is working...");
  start_turn(std::move(message), std::move(turn_attachments));
}

void App::start_turn(std::string message, tokmon::Json attachments) {
  if (active_turn_.joinable())
    active_turn_.join();
  active_turn_ = std::jthread([this, message = std::move(message),
                               attachments = std::move(attachments)](
                                  std::stop_token stop) {
    std::string final_status;
    try {
      if (snow_process_) {
        const auto result =
            snow_process_->request("turn.start",
                                   {{"session_id", session_.str()},
                                    {"message", message},
                                    {"model", config_.model},
                                    {"attachments", attachments},
                                    {"max_steps", config_.max_steps}},
                                   config_.request_timeout);
        final_status = "Turn " + result.value("reason", "completed");
        const auto events = snow_process_->request(
            "session.events",
            {{"session_id", session_.str()}, {"after", projection_->cursor()}},
            std::chrono::seconds(5));
        apply_events(events);
      } else {
        const auto result =
            embedded_snow_->agent().run(session_, message,
                                        {.model = config_.model,
                                         .attachments = attachments,
                                         .max_steps = config_.max_steps},
                                        stop);
        final_status = "Turn " + std::string(snow::to_string(result.reason));
      }
    } catch (const tokmon::Error &error) {
      final_status = error.code() + ": " + error.what();
    } catch (const std::exception &error) {
      final_status = "Turn failed: " + std::string(error.what());
    }
    {
      std::lock_guard lock(state_mutex_);
      turn_active_ = false;
    }
    update_status(std::move(final_status));
    refresh_sessions();
    if (window_)
      window_->invalidate();
  });
}

void App::connect_child(bool initial) {
  if (!snow_process_)
    return;
  snow_process_->start();
  const auto initialized = snow_process_->initialize();
  if (initialized.value("selected_protocol", 0) != 1) {
    throw tokmon::Error("tokmon.snow.protocol",
                        "Snow selected an unsupported protocol");
  }

  if (!session_.empty()) {
    try {
      const auto resumed = snow_process_->request(
          "session.resume",
          {{"session_id", session_.str()}, {"after", projection_->cursor()}},
          std::chrono::seconds(10));
      apply_events(resumed.value("events", tokmon::Json::array()));
    } catch (const tokmon::Error &error) {
      if (error.code() != "snow.session.missing")
        throw;
      session_ = {};
      projection_->clear();
    }
  }
  if (session_.empty()) {
    const auto created = snow_process_->request(
        "session.create",
        {{"metadata",
          {{"origin", "tokmon-desktop"}, {"transport", "child-process"}}}},
        std::chrono::seconds(10));
    session_ = tokmon::SessionId(created.at("session_id").get<std::string>());
    persist_session();
  }
  update_status(std::string(initial ? "Snow connected" : "Snow reconnected") +
                " (PID " + std::to_string(snow_process_->process_id()) + ")");
  if (!initial)
    refresh_sessions();
}

void App::poll_child(std::stop_token stop) {
  while (!stop.stop_requested()) {
    if (snow_process_ && snow_process_->alive() && !session_.empty()) {
      try {
        const auto events = snow_process_->request(
            "session.events",
            {{"session_id", session_.str()}, {"after", projection_->cursor()}},
            std::chrono::seconds(5));
        apply_events(events);
      } catch (const tokmon::Error &error) {
        if (error.code() != "tokmon.snow.disconnected" &&
            error.code() != "tokmon.snow.write" &&
            error.code() != "tokmon.snow.timeout") {
          update_status(error.code() + ": " + error.what());
        }
      }
    }
    std::this_thread::sleep_for(config_.poll_interval);
  }
}

void App::supervise_child(std::stop_token stop) {
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(state_mutex_);
      supervisor_ready_.wait(
          lock, stop, [this] { return restart_requested_ || shutting_down_; });
      if (stop.stop_requested() || shutting_down_)
        return;
      restart_requested_ = false;
    }
    for (std::size_t attempt = 1;
         attempt <= config_.restart_max_attempts && !stop.stop_requested();
         ++attempt) {
      const auto delay = config_.restart_base_delay *
                         static_cast<int>(std::min<std::size_t>(
                             std::size_t{16}, std::size_t{1} << (attempt - 1)));
      std::this_thread::sleep_for(delay);
      try {
        snow_process_->stop();
        connect_child(false);
        break;
      } catch (const std::exception &error) {
        update_status("Snow restart " + std::to_string(attempt) + "/" +
                      std::to_string(config_.restart_max_attempts) +
                      " failed: " + error.what());
        if (attempt == config_.restart_max_attempts) {
          update_status("Snow restart budget exhausted; use /restart");
        }
      }
    }
  }
}

void App::apply_events(const tokmon::Json &events) {
  if (!events.is_array())
    return;
  bool changed = false;
  for (const auto &value : events) {
    const auto event = value.get<snow::TrajectoryEvent>();
    if (event.session_id == session_ && event.seq > projection_->cursor()) {
      projection_->apply(event);
      changed = true;
    }
  }
  if (changed && window_)
    window_->invalidate();
}

void App::handle_notification(const tokmon::Json &notification) {
  if (notification.value("method", "") != "approval.request")
    return;
  const auto params = notification.value("params", tokmon::Json::object());
  const auto approval_id = params.value("approval_id", "");
  if (approval_id.empty())
    return;
  snow::ToolDefinition tool;
  tool.name = params.value("tool", "unknown");
  tool.description = "Remote Snow tool approval";
  tool.input_schema = tokmon::Json::object();
  approvals_->present(
      {approval_id,
       std::move(tool),
       params.value("arguments", tokmon::Json::object()),
       params.value("reason", "policy requires approval"),
       {.canonical_plan_hash = params.value("canonical_plan_hash", ""),
        .idempotency_key = params.value("idempotency_key", ""),
        .sandbox_plan = params.value("sandbox_plan", tokmon::Json::object())}},
      [this](std::string id, bool approved) {
        try {
          (void)snow_process_->request(
              "approval.respond", {{"approval_id", id}, {"approved", approved}},
              std::chrono::seconds(5));
        } catch (const std::exception &error) {
          update_status("Approval response failed: " +
                        std::string(error.what()));
        }
      });
}

void App::update_status(std::string status) {
  {
    std::lock_guard lock(state_mutex_);
    status_ = std::move(status);
    status = status_;
  }
  if (window_) {
    window_->set_status(std::move(status));
    window_->invalidate();
  }
}

void App::handle_editor_submit(std::string value) {
  InputMode mode;
  {
    std::lock_guard lock(state_mutex_);
    mode = input_mode_;
    if (mode == InputMode::filter)
      file_filter_ = value;
    else if (mode == InputMode::trajectory_search)
      trajectory_search_ = value;
    else if (mode == InputMode::settings) {
      if (auto *field = editable_setting(settings_, active_settings_field_))
        *field = value;
    } else {
      message_draft_.clear();
    }
  }
  if (mode != InputMode::message) {
    window_->set_input_text(std::move(value));
    window_->set_input_focused(true);
    window_->invalidate();
    return;
  }
  submit(std::move(value));
}

void App::set_input_mode(InputMode mode, std::string settings_field) {
  const auto editor = window_->editor_snapshot();
  std::string next_value;
  {
    std::lock_guard lock(state_mutex_);
    if (input_mode_ == InputMode::filter)
      file_filter_ = editor.value;
    else if (input_mode_ == InputMode::trajectory_search)
      trajectory_search_ = editor.value;
    else if (input_mode_ == InputMode::settings) {
      if (auto *field = editable_setting(settings_, active_settings_field_))
        *field = editor.value;
    } else
      message_draft_ = editor.value;

    input_mode_ = mode;
    active_settings_field_ =
        mode == InputMode::settings ? std::move(settings_field) : "";
    if (mode == InputMode::filter)
      next_value = file_filter_;
    else if (mode == InputMode::trajectory_search)
      next_value = trajectory_search_;
    else if (mode == InputMode::settings) {
      if (const auto *field =
              editable_setting(settings_, active_settings_field_))
        next_value = *field;
    } else
      next_value = message_draft_;
  }
  window_->set_input_text(std::move(next_value));
  window_->set_input_focused(true);
  window_->invalidate();
}

void App::apply_setting(std::string value, std::size_t index) {
  const auto editor = window_->editor_snapshot();
  std::lock_guard lock(state_mutex_);
  if (input_mode_ == InputMode::settings) {
    if (auto *field = editable_setting(settings_, active_settings_field_))
      *field = editor.value;
  }
  if (value == "plugin") {
    if (index < settings_.plugins.size() && !settings_.plugins[index].required)
      settings_.plugins[index].disabled = !settings_.plugins[index].disabled;
    window_->invalidate();
    return;
  }
  const auto separator = value.find('=');
  if (separator == std::string::npos)
    return;
  const auto key = value.substr(0, separator);
  const auto selected = value.substr(separator + 1);
  if (key == "language")
    settings_.language = selected;
  else if (key == "theme")
    settings_.theme = selected;
  else if (key == "provider_template") {
    settings_.provider_id = "custom_provider";
    settings_.provider_name = "Custom provider";
    settings_.provider_kind = "openai-compatible";
    settings_.endpoint.clear();
    settings_.api_key_env = "CUSTOM_API_KEY";
    settings_.model.clear();
  } else if (key == "agent_preset") {
    settings_.agent_preset = selected;
    if (selected == "autonomous")
      settings_.max_steps = "96";
    else if (selected == "review")
      settings_.max_steps = "16";
    else
      settings_.max_steps = "32";
  } else if (key == "default_permission")
    settings_.default_permission = selected;
  else if (key == "auto_scroll")
    settings_.auto_scroll = selected == "true";
  else if (key == "raw_trace")
    settings_.raw_trace = selected == "true";
  else if (key == "restart_enabled")
    settings_.restart_enabled = selected == "true";
  window_->invalidate();
}

void App::export_trajectory() {
  try {
    const auto events = projection_->event_snapshot();
    tokmon::Json document = {{"schema", "org.tokmon.snow.session-log/v1"},
                             {"session_id", session_.str()},
                             {"exported_at", tokmon::iso8601()},
                             {"events", events}};
    auto stamp = tokmon::iso8601();
    std::ranges::replace_if(
        stamp,
        [](char character) { return character == ':' || character == '.'; },
        '-');
    const auto path = config_.workspace / config_.config_dir_name / "exports" /
                      ("session-" + session_.str() + "-" + stamp + ".json");
    tokmon::write_text_file_atomic(path, document.dump(2) + "\n");
    (void)workbench_.show_document(path);
    update_status("Session log exported to " + path.string());
  } catch (const std::exception &error) {
    update_status("Session log export failed: " + std::string(error.what()));
  }
}

void App::refresh_sessions() {
  try {
    std::vector<WorkbenchSession> result;
    if (snow_process_) {
      const auto value = snow_process_->request(
          "session.list", {{"limit", 100}}, std::chrono::seconds(10));
      if (value.is_array()) {
        for (const auto &item : value) {
          const auto id = item.value("session_id", "");
          if (id.empty())
            continue;
          const auto header = item.value("header", tokmon::Json::object());
          auto title = header.value("title", "");
          if (title.empty())
            title = "新会话";
          result.push_back({id, std::move(title), item.value("created_at", ""),
                            item.value("last_seq", std::uint64_t{0}),
                            item.value("closed", false)});
        }
      }
    } else if (embedded_snow_) {
      for (const auto &item : embedded_snow_->agent().sessions(100)) {
        auto title = item.header.value("title", "");
        if (title.empty())
          title = "新会话";
        result.push_back({item.id.str(), std::move(title), item.created_at,
                          item.last_seq, item.closed_at.has_value()});
      }
    }
    {
      std::lock_guard lock(state_mutex_);
      sessions_ = std::move(result);
      ++sessions_revision_;
    }
    if (window_)
      window_->invalidate();
  } catch (const std::exception &error) {
    update_status("Session list failed: " + std::string(error.what()));
  }
}

void App::switch_session(std::string session_id) {
  if (session_id.empty() || session_id == session_.str())
    return;
  {
    std::lock_guard lock(state_mutex_);
    if (turn_active_) {
      status_ = "Finish or cancel the active turn before switching sessions";
      window_->set_status(status_);
      window_->invalidate();
      return;
    }
  }
  try {
    std::vector<snow::TrajectoryEvent> events;
    if (snow_process_) {
      const auto resumed = snow_process_->request(
          "session.resume", {{"session_id", session_id}, {"after", 0}},
          std::chrono::seconds(10));
      for (const auto &value : resumed.value("events", tokmon::Json::array()))
        events.push_back(value.get<snow::TrajectoryEvent>());
    } else {
      events = embedded_snow_->agent().events(tokmon::SessionId(session_id));
    }
    session_ = tokmon::SessionId(std::move(session_id));
    projection_->replay(events);
    persist_session();
    update_status("Switched to session " + session_.str());
  } catch (const std::exception &error) {
    update_status("Session switch failed: " + std::string(error.what()));
  }
}

void App::choose_attachments() {
  window_->choose_files(
      [this](std::vector<std::filesystem::path> files) {
        constexpr std::uintmax_t per_file_limit = 1024U * 1024U;
        constexpr std::uintmax_t total_limit = 2U * 1024U * 1024U;
        std::size_t added = 0;
        std::string rejection;
        for (const auto &selected : files) {
          std::error_code error;
          const auto path = std::filesystem::weakly_canonical(selected, error);
          if (error || !std::filesystem::is_regular_file(path, error)) {
            rejection = "Skipped a missing or invalid attachment";
            continue;
          }
          const auto size = std::filesystem::file_size(path, error);
          if (error || size > per_file_limit) {
            rejection = "Each attachment must be 1 MiB or smaller";
            continue;
          }
          std::string content;
          try {
            content = tokmon::read_text_file(path);
          } catch (const std::exception &read_error) {
            rejection = read_error.what();
            continue;
          }
          if (content.find('\0') != std::string::npos) {
            rejection = "Binary attachments are not supported";
            continue;
          }
          std::lock_guard lock(state_mutex_);
          std::size_t total = 0;
          for (const auto &file : attachments_)
            total += file.content.size();
          if (attachments_.size() >= 8 ||
              total + content.size() > total_limit) {
            rejection = "Attachments are limited to 8 files and 2 MiB total";
            continue;
          }
          if (std::ranges::any_of(attachments_, [&](const AttachedFile &file) {
                return file.path == path;
              }))
            continue;
          attachments_.push_back({path, path.filename().string(), content,
                                  tokmon::sha256_hex(content)});
          ++added;
        }
        if (added > 0)
          update_status("Added " + std::to_string(added) + " attachment(s)");
        else if (!rejection.empty())
          update_status(std::move(rejection));
        if (window_)
          window_->invalidate();
      },
      true, config_.workspace);
}

void App::choose_document() {
  window_->choose_files(
      [this](std::vector<std::filesystem::path> files) {
        if (files.empty())
          return;
        if (!workbench_.show_document(files.front())) {
          update_status("Document preview accepts text/source files inside the "
                        "workspace");
          return;
        }
        update_status("Opened " + files.front().filename().string());
        if (window_)
          window_->invalidate();
      },
      false, config_.workspace);
}

bool App::handle_workbench_event(const white::UiEvent &event) {
  const auto action = workbench_.dispatch(event);
  if (action.pointer_cursor)
    window_->set_pointer_cursor(*action.pointer_cursor);
  switch (action.kind) {
  case WorkbenchActionKind::new_session:
    submit("/new");
    break;
  case WorkbenchActionKind::fork_session:
    submit("/fork");
    break;
  case WorkbenchActionKind::diagnostics:
    submit("/diagnostics");
    break;
  case WorkbenchActionKind::inspect_composition:
    submit("/inspect");
    break;
  case WorkbenchActionKind::cancel_turn:
    submit("/cancel");
    break;
  case WorkbenchActionKind::submit_input:
    set_input_mode(InputMode::message);
    window_->submit_input();
    break;
  case WorkbenchActionKind::set_message_input:
    set_input_mode(InputMode::message);
    {
      std::lock_guard lock(state_mutex_);
      message_draft_ = action.value;
    }
    window_->set_input_text(action.value);
    window_->set_input_focused(true);
    break;
  case WorkbenchActionKind::copy_text:
    try {
      window_->copy_to_clipboard(action.value);
      update_status("Copied message to clipboard");
    } catch (const std::exception &error) {
      update_status("Copy failed: " + std::string(error.what()));
    }
    break;
  case WorkbenchActionKind::focus_message:
    set_input_mode(InputMode::message);
    window_->set_input_cursor(action.cursor, false);
    break;
  case WorkbenchActionKind::focus_filter:
    set_input_mode(InputMode::filter);
    window_->set_input_cursor(action.cursor, false);
    break;
  case WorkbenchActionKind::set_editor_cursor:
    window_->set_input_cursor(action.cursor, action.extend_selection);
    break;
  case WorkbenchActionKind::switch_session:
    switch_session(action.value);
    break;
  case WorkbenchActionKind::attach_files:
    choose_attachments();
    break;
  case WorkbenchActionKind::open_file_dialog:
    choose_document();
    break;
  case WorkbenchActionKind::remove_attachment: {
    std::lock_guard lock(state_mutex_);
    if (action.index < attachments_.size())
      attachments_.erase(attachments_.begin() +
                         static_cast<std::ptrdiff_t>(action.index));
    window_->invalidate();
    break;
  }
  case WorkbenchActionKind::show_help:
    submit("/help");
    break;
  case WorkbenchActionKind::approve:
    approvals_->resolve(true);
    break;
  case WorkbenchActionKind::deny:
    approvals_->resolve(false);
    break;
  case WorkbenchActionKind::window_minimize:
    window_->minimize();
    break;
  case WorkbenchActionKind::window_toggle_maximize:
    window_->toggle_maximize();
    window_->invalidate();
    break;
  case WorkbenchActionKind::window_close:
    window_->close();
    break;
  case WorkbenchActionKind::open_settings: {
    auto loaded =
        load_desktop_settings(config_.workspace, config_.config_dir_name);
    {
      std::lock_guard lock(state_mutex_);
      settings_ = std::move(loaded);
    }
    set_input_mode(InputMode::settings);
    break;
  }
  case WorkbenchActionKind::close_settings: {
    set_input_mode(InputMode::message);
    auto loaded =
        load_desktop_settings(config_.workspace, config_.config_dir_name);
    {
      std::lock_guard lock(state_mutex_);
      settings_ = std::move(loaded);
    }
    window_->invalidate();
    break;
  }
  case WorkbenchActionKind::save_settings: {
    set_input_mode(InputMode::message);
    DesktopSettings saved;
    {
      std::lock_guard lock(state_mutex_);
      saved = settings_;
    }
    try {
      save_desktop_settings(config_.workspace, saved);
      config_.model = saved.model;
      config_.raw_trace = saved.raw_trace;
      config_.restart_enabled = saved.restart_enabled;
      config_.request_timeout = std::chrono::milliseconds(
          setting_number(saved.request_timeout_ms, 300000, 1000, 3600000));
      config_.max_steps = setting_number(saved.max_steps, 32, 1, 1024);
      workbench_.close_settings();
      update_status("设置已保存；提供方与插件变更将在 Snow 下次启动时生效");
    } catch (const std::exception &error) {
      update_status("保存设置失败: " + std::string(error.what()));
    }
    break;
  }
  case WorkbenchActionKind::open_config_file: {
    set_input_mode(InputMode::message);
    const auto path =
        config_.workspace / config_.config_dir_name / "tokmon.json";
    try {
      if (!std::filesystem::exists(path)) {
        DesktopSettings current;
        {
          std::lock_guard lock(state_mutex_);
          current = settings_;
        }
        save_desktop_settings(config_.workspace, current);
      }
      if (workbench_.show_document(path))
        update_status("已打开 " + path.string());
      else
        update_status("无法在工作区预览配置文件");
    } catch (const std::exception &error) {
      update_status("打开配置失败: " + std::string(error.what()));
    }
    break;
  }
  case WorkbenchActionKind::focus_settings_field:
    set_input_mode(InputMode::settings, action.value);
    window_->set_input_cursor(action.cursor, false);
    break;
  case WorkbenchActionKind::set_setting:
    apply_setting(action.value, action.index);
    break;
  case WorkbenchActionKind::settings_tab:
    set_input_mode(InputMode::settings);
    break;
  case WorkbenchActionKind::focus_trajectory_search:
    set_input_mode(InputMode::trajectory_search);
    window_->set_input_cursor(action.cursor, false);
    break;
  case WorkbenchActionKind::show_trajectory:
    set_input_mode(InputMode::trajectory_search);
    break;
  case WorkbenchActionKind::show_conversation:
    set_input_mode(InputMode::message);
    break;
  case WorkbenchActionKind::export_trajectory:
    export_trajectory();
    break;
  case WorkbenchActionKind::redraw:
    window_->invalidate();
    break;
  default:
    break;
  }
  return action.kind != WorkbenchActionKind::none;
}

void App::persist_session() const {
  if (session_.empty())
    return;
  const auto path = config_.workspace / config_.config_dir_name / "state.json";
  tokmon::write_text_file_atomic(path,
                                 tokmon::Json{{"schema", 1},
                                              {"session_id", session_.str()},
                                              {"updated_at", tokmon::iso8601()}}
                                     .dump(2));
}

void App::draw(white::RasterSurface &surface) {
  const auto editor = window_->editor_snapshot();
  const auto projection_revision = projection_->revision();
  if (projection_revision != cached_projection_revision_) {
    auto snapshot = projection_->snapshot_all();
    cached_projection_items_ = std::move(snapshot.items);
    cached_projection_events_ = std::move(snapshot.events);
    cached_projection_cursor_ = snapshot.cursor;
    cached_projection_revision_ = snapshot.revision;
  }
  WorkbenchFrame frame;
  frame.item_source = &cached_projection_items_;
  frame.trajectory_event_source = &cached_projection_events_;
  frame.approval = approvals_->pending();
  frame.session_id = session_.str();
  frame.model = config_.model;
  frame.trajectory_cursor = cached_projection_cursor_;
  frame.composition_epoch = ui_runtime_.epoch();
  frame.snow_connected =
      embedded_snow_ != nullptr || (snow_process_ && snow_process_->alive());
  frame.window_maximized = window_->maximized();
  {
    std::lock_guard lock(state_mutex_);
    frame.status = status_;
    frame.turn_active = turn_active_;
    if (input_mode_ == InputMode::filter)
      file_filter_ = editor.value;
    else if (input_mode_ == InputMode::trajectory_search)
      trajectory_search_ = editor.value;
    else if (input_mode_ == InputMode::settings) {
      if (auto *field = editable_setting(settings_, active_settings_field_))
        *field = editor.value;
    } else
      message_draft_ = editor.value;
    frame.message_input = message_draft_;
    frame.file_filter = file_filter_;
    frame.trajectory_search = trajectory_search_;
    frame.settings = settings_;
    frame.active_settings_field = active_settings_field_;
    if (!editor.composition.empty()) {
      std::string *active_text = nullptr;
      if (input_mode_ == InputMode::filter)
        active_text = &frame.file_filter;
      else if (input_mode_ == InputMode::trajectory_search)
        active_text = &frame.trajectory_search;
      else if (input_mode_ == InputMode::settings)
        active_text =
            editable_setting(frame.settings, frame.active_settings_field);
      else
        active_text = &frame.message_input;
      if (active_text)
        active_text->insert(std::min(editor.cursor, active_text->size()),
                            editor.composition);
    }
    if (cached_sessions_revision_ != sessions_revision_) {
      cached_sessions_ = sessions_;
      cached_sessions_revision_ = sessions_revision_;
    }
    frame.session_source = &cached_sessions_;
    frame.attachments.reserve(attachments_.size());
    for (const auto &attachment : attachments_)
      frame.attachments.push_back({attachment.name, attachment.content.size()});
    frame.message_focused = input_mode_ == InputMode::message && editor.focused;
    frame.filter_focused = input_mode_ == InputMode::filter && editor.focused;
    frame.trajectory_search_focused =
        input_mode_ == InputMode::trajectory_search && editor.focused;
    frame.settings_field_focused = input_mode_ == InputMode::settings &&
                                   editor.focused &&
                                   !active_settings_field_.empty();
  }
  frame.editor_cursor = editor.cursor + editor.composition.size();
  frame.selection_start = editor.selection_start;
  frame.selection_end = editor.selection_end;
  frame.caret_visible = editor.caret_visible;
  workbench_.draw(surface, frame);
}

std::shared_ptr<snow::ModelProvider> App::create_model() const {
  const auto layout =
      snow::ConfigLayout::resolve(config_.workspace, config_.config_dir_name);
  const auto document = snow::load_providers_config(layout);
  const auto provider = document.value("default", tokmon::Json::object());
  const auto endpoint = provider.value("endpoint", "");
  if (!endpoint.empty()) {
    std::string api_key;
    if (const auto value =
            tokmon::environment_variable(provider.value("api_key_env", "")))
      api_key = *value;
    return std::make_shared<snow::OpenAICompatibleProvider>(
        snow::OpenAICompatibleConfig{
            .endpoint = endpoint,
            .api_key = std::move(api_key),
            .model = provider.value("model", config_.model)});
  }
  return std::make_shared<snow::ScriptedModelProvider>(
      std::vector<snow::ModelResponse>{
          {.content =
               "No model provider is configured. Add providers.json under " +
               config_.config_dir_name + "."}});
}

} // namespace tokmon::desktop
