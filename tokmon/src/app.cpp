#include <tokmon/app.hpp>

#include <snow/config.hpp>
#include <tokmon/common/files.hpp>
#include <white/virtual_list.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

namespace tokmon::desktop {
namespace {

std::string wrap(std::string_view text, std::size_t width,
                 std::size_t line) {
  const auto start = line * width;
  if (start >= text.size()) return {};
  return std::string(text.substr(start, width));
}

std::filesystem::path resolve_config_path(
    const std::filesystem::path& workspace,
    const std::filesystem::path& default_value,
    const tokmon::Json& document, std::string_view key) {
  const auto configured = document.value(std::string(key), "");
  if (configured.empty()) return default_value;
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
    if (character == '=') break;
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
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  }
  return "Binary artifact (" + std::to_string(bytes.size()) +
         " bytes, " + std::string(media_type) + ")";
}

} // namespace

AppConfig load_app_config(const std::filesystem::path& workspace,
                          std::string config_dir_name) {
  AppConfig result;
  result.workspace = std::filesystem::weakly_canonical(workspace);
  result.config_dir_name = std::move(config_dir_name);
  const auto layout =
      snow::ConfigLayout::resolve(result.workspace, result.config_dir_name);
  const auto path = layout.config_root / "tokmon.json";
  const auto document =
      snow::load_json_config(path, tokmon::Json::object());
  if (!document.empty() &&
      document.value("schema", "") != "org.tokmon.desktop.config/v1")
    throw tokmon::Error("tokmon.config.schema",
                        "invalid tokmon.json schema");
  const auto snow_config =
      document.value("snow", tokmon::Json::object());
  const auto ui_config = document.value("ui", tokmon::Json::object());
  const auto restart =
      snow_config.value("restart", tokmon::Json::object());

  result.data_root = resolve_config_path(
      result.workspace, layout.config_root / "data", document, "data_root");
  result.snow_executable = resolve_config_path(
      result.workspace, SnowProcessClient::sibling_snow_executable(),
      snow_config, "executable");
  result.embedded_snow = snow_config.value("mode", "process") == "embedded";
  result.raw_trace = snow_config.value("raw_trace", false);
  result.restart_enabled = restart.value("enabled", true);
  result.restart_max_attempts = restart.value("max_attempts", 5U);
  result.restart_base_delay = std::chrono::milliseconds(
      restart.value("base_delay_ms", 250U));
  result.poll_interval =
      std::chrono::milliseconds(ui_config.value("poll_interval_ms", 25U));
  result.request_timeout = std::chrono::milliseconds(
      snow_config.value("request_timeout_ms", 300000U));
  result.model = document.value("model", "");
  if (result.restart_max_attempts == 0) result.restart_max_attempts = 1;
  if (result.poll_interval < std::chrono::milliseconds(5))
    result.poll_interval = std::chrono::milliseconds(5);
  return result;
}

App::App(AppConfig config)
    : config_(std::move(config)),
      white_(ui_runtime_),
      approvals_(std::make_shared<ApprovalCoordinator>()),
      projection_(std::make_shared<Projection>()) {
  window_ = white_.service().create_window(
      {.title = "Tokmon", .width = 1280, .height = 820});
  window_->set_draw_callback(
      [this](white::RasterSurface& surface) { draw(surface); });
  window_->set_submit_callback(
      [this](std::string message) { submit(std::move(message)); });
  approvals_->set_changed([this] {
    if (window_) window_->invalidate();
  });

  const auto state_path =
      config_.workspace / config_.config_dir_name / "state.json";
  if (std::filesystem::exists(state_path)) {
    const auto state = snow::load_json_config(state_path, tokmon::Json::object());
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
      } catch (const tokmon::Error& error) {
        if (error.code() != "snow.session.missing") throw;
        session_ = {};
      }
    }
    if (session_.empty()) {
      session_ = embedded_snow_->agent().create_session(
          {{"origin", "tokmon-desktop"}, {"mode", "embedded"}});
      persist_session();
    }
    auto journal = embedded_snow_->arche_runtime().root_context()
                       ->require<snow::TrajectoryJournal>("snow.trajectory",
                                                          "^1.0");
    event_connection_ = journal->committed().connect(
        [this](const snow::TrajectoryEvent& event) {
          if (event.session_id == session_) {
            projection_->apply(event);
            if (window_) window_->invalidate();
          }
        });
    update_status("Snow embedded runtime ready");
  } else {
    snow_process_ = std::make_shared<SnowProcessClient>(SnowProcessOptions{
        .executable = config_.snow_executable,
        .workspace = config_.workspace,
        .data_root = config_.data_root,
        .config_dir_name = config_.config_dir_name,
        .raw_trace = config_.raw_trace,
        .request_timeout = config_.request_timeout});
    snow_process_->set_notification_handler(
        [this](const tokmon::Json& value) { handle_notification(value); });
    snow_process_->set_crash_handler([this](const SnowProcessExit& exit) {
      if (exit.expected) return;
      {
        std::lock_guard lock(state_mutex_);
        if (shutting_down_) return;
        restart_requested_ = config_.restart_enabled;
        turn_active_ = false;
      }
      update_status("Snow exited with code " + std::to_string(exit.code));
      supervisor_ready_.notify_all();
    });
    connect_child(true);
    supervisor_ = std::jthread(
        [this](std::stop_token stop) { supervise_child(stop); });
    poller_ =
        std::jthread([this](std::stop_token stop) { poll_child(stop); });
  }
  product_ = std::make_unique<ProductAssembly>(
      ui_runtime_, projection_, approvals_, snow_process_);
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
  if (snow_process_) snow_process_->stop();
}

int App::run() { return window_->run(); }

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
      } catch (const std::exception& error) {
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
    } catch (const std::exception& error) {
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
            {{"session_id", session_.str()}, {"seed_seq", seed},
             {"metadata", {{"origin", "tokmon-desktop"}}}},
            std::chrono::seconds(10));
        session_ = tokmon::SessionId(
            forked.at("session_id").get<std::string>());
      } else {
        session_ = embedded_snow_->agent().fork_session(
            session_, {{"origin", "tokmon-desktop"}}, seed);
      }
      projection_->begin_fork();
      persist_session();
      update_status("Forked session " + session_.str());
    } catch (const std::exception& error) {
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
        "/cancel\n/steer <message>\n/fork\n/restart\n/diagnostics\n"
        "/inspect\n/replay [R0|R1|R2]\n"
        "/artifact <sha256> [media-type]");
    window_->invalidate();
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
        value = snow_process_
                    ? snow_process_->request("composition.inspect", {},
                                             std::chrono::seconds(5))
                    : tokmon::Json{
                          {"epoch", embedded_snow_->arche_runtime().epoch()},
                          {"runtime",
                           embedded_snow_->arche_runtime().inspect()}};
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
              session_, level == "R0"
                            ? snow::ReplayLevel::transcript
                            : (level == "R1"
                                   ? snow::ReplayLevel::request_reconstruction
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
    } catch (const std::exception& error) {
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
      if (sha256.starts_with("sha256:")) sha256.erase(0, 7);
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
          const auto chunk = snow_process_->request(
              "artifact.read",
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
        auto artifacts = embedded_snow_->arche_runtime().root_context()
                             ->require<snow::ArtifactStore>("snow.artifacts",
                                                            "^1.0");
        bytes = artifacts->read(
            {.id = "sha256:" + sha256,
             .sha256 = sha256,
             .media_type = media_type});
      }
      projection_->append_local(
          ItemKind::artifact, "Artifact / " + sha256.substr(0, 12),
          artifact_preview(bytes, media_type), "loaded",
          {{"sha256", sha256},
           {"media_type", media_type},
           {"bytes", bytes.size()}});
      window_->invalidate();
    } catch (const std::exception& error) {
      update_status("Artifact read failed: " + std::string(error.what()));
    }
    return;
  }

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
  }
  update_status("Snow is working...");
  start_turn(std::move(message));
}

void App::start_turn(std::string message) {
  if (active_turn_.joinable()) active_turn_.join();
  active_turn_ = std::jthread(
      [this, message = std::move(message)](std::stop_token stop) {
        std::string final_status;
        try {
          if (snow_process_) {
            const auto result = snow_process_->request(
                "turn.start",
                {{"session_id", session_.str()},
                 {"message", message},
                 {"model", config_.model},
                 {"max_steps", 32}},
                config_.request_timeout);
            final_status = "Turn " + result.value("reason", "completed");
            const auto events = snow_process_->request(
                "session.events",
                {{"session_id", session_.str()},
                 {"after", projection_->cursor()}},
                std::chrono::seconds(5));
            apply_events(events);
          } else {
            const auto result = embedded_snow_->agent().run(
                session_, message,
                {.model = config_.model, .max_steps = 32}, stop);
            final_status =
                "Turn " + std::string(snow::to_string(result.reason));
          }
        } catch (const tokmon::Error& error) {
          final_status = error.code() + ": " + error.what();
        } catch (const std::exception& error) {
          final_status = "Turn failed: " + std::string(error.what());
        }
        {
          std::lock_guard lock(state_mutex_);
          turn_active_ = false;
        }
        update_status(std::move(final_status));
        if (window_) window_->invalidate();
      });
}

void App::connect_child(bool initial) {
  if (!snow_process_) return;
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
    } catch (const tokmon::Error& error) {
      if (error.code() != "snow.session.missing") throw;
      session_ = {};
      projection_->clear();
    }
  }
  if (session_.empty()) {
    const auto created = snow_process_->request(
        "session.create",
        {{"metadata", {{"origin", "tokmon-desktop"},
                        {"transport", "child-process"}}}},
        std::chrono::seconds(10));
    session_ = tokmon::SessionId(created.at("session_id").get<std::string>());
    persist_session();
  }
  update_status(std::string(initial ? "Snow connected" : "Snow reconnected") +
                " (PID " + std::to_string(snow_process_->process_id()) + ")");
}

void App::poll_child(std::stop_token stop) {
  while (!stop.stop_requested()) {
    if (snow_process_ && snow_process_->alive() && !session_.empty()) {
      try {
        const auto events = snow_process_->request(
            "session.events",
            {{"session_id", session_.str()},
             {"after", projection_->cursor()}},
            std::chrono::seconds(5));
        apply_events(events);
      } catch (const tokmon::Error& error) {
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
      supervisor_ready_.wait(lock, stop, [this] {
        return restart_requested_ || shutting_down_;
      });
      if (stop.stop_requested() || shutting_down_) return;
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
      } catch (const std::exception& error) {
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

void App::apply_events(const tokmon::Json& events) {
  if (!events.is_array()) return;
  bool changed = false;
  for (const auto& value : events) {
    const auto event = value.get<snow::TrajectoryEvent>();
    if (event.session_id == session_ && event.seq > projection_->cursor()) {
      projection_->apply(event);
      changed = true;
    }
  }
  if (changed && window_) window_->invalidate();
}

void App::handle_notification(const tokmon::Json& notification) {
  if (notification.value("method", "") != "approval.request") return;
  const auto params = notification.value("params", tokmon::Json::object());
  const auto approval_id = params.value("approval_id", "");
  if (approval_id.empty()) return;
  snow::ToolDefinition tool;
  tool.name = params.value("tool", "unknown");
  tool.description = "Remote Snow tool approval";
  tool.input_schema = tokmon::Json::object();
  approvals_->present(
      {approval_id, std::move(tool),
       params.value("arguments", tokmon::Json::object()),
       params.value("reason", "policy requires approval"),
       {.canonical_plan_hash = params.value("canonical_plan_hash", ""),
        .idempotency_key = params.value("idempotency_key", ""),
        .sandbox_plan =
            params.value("sandbox_plan", tokmon::Json::object())}},
      [this](std::string id, bool approved) {
        try {
          (void)snow_process_->request(
              "approval.respond",
              {{"approval_id", id}, {"approved", approved}},
              std::chrono::seconds(5));
        } catch (const std::exception& error) {
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

void App::persist_session() const {
  if (session_.empty()) return;
  const auto path =
      config_.workspace / config_.config_dir_name / "state.json";
  tokmon::write_text_file_atomic(
      path, tokmon::Json{{"schema", 1},
                         {"session_id", session_.str()},
                         {"updated_at", tokmon::iso8601()}}
                .dump(2));
}

void App::draw(white::RasterSurface& surface) {
  const auto items = projection_->snapshot();
  const float width = static_cast<float>(surface.width());
  float y = 52;
  constexpr float estimated_height = 126;
  white::VirtualList list;
  list.configure(items.size(), estimated_height,
                 static_cast<float>(surface.height()) - 120, 2);
  list.scroll_to_end();
  const auto [first, end] = list.visible_range();
  for (std::size_t index = first; index < end; ++index) {
    const auto& item = items[index];
    const bool user = item.kind == ItemKind::user;
    const auto background =
        user ? white::Color{225, 233, 255, 255}
             : (item.kind == ItemKind::error
                    ? white::Color{255, 230, 230, 255}
                    : (item.kind == ItemKind::artifact
                           ? white::Color{231, 248, 241, 255}
                           : (item.kind == ItemKind::diagnostic
                                  ? white::Color{239, 237, 255, 255}
                                  : white::Color{255, 255, 255, 255}))
                    );
    const float x = user ? width * 0.30F : 24;
    const float box_width = user ? width * 0.66F : width * 0.78F;
    const auto lines =
        std::max<std::size_t>(1, (item.content.size() + 89) / 90);
    const float box_height =
        48 + static_cast<float>(std::min<std::size_t>(lines, 5)) * 19;
    surface.fill_rect({x, y, box_width, box_height}, background, 10);
    surface.stroke_rect({x, y, box_width, box_height},
                        {220, 223, 230, 255}, 1, 10);
    surface.text(item.title + " / " + item.status, x + 14, y + 21, 13,
                 {90, 96, 108, 255});
    for (std::size_t line = 0; line < std::min<std::size_t>(lines, 5);
         ++line) {
      surface.text(wrap(item.content, 90, line), x + 14,
                   y + 46 + static_cast<float>(line) * 19, 15,
                   {25, 27, 32, 255});
    }
    y += box_height + 12;
    if (y > static_cast<float>(surface.height()) - 90) break;
  }
  if (const auto pending = approvals_->pending()) {
    surface.fill_rect({width - 340, 40, 316, 206},
                      {255, 247, 220, 255}, 10);
    surface.stroke_rect({width - 340, 40, 316, 206},
                        {222, 173, 70, 255}, 1, 10);
    surface.text("Approval required: " + pending->tool.name, width - 322, 68,
                 16, {70, 50, 15, 255});
    surface.text("Type /approve or /deny", width - 322, 94, 14,
                 {90, 65, 20, 255});
    surface.text(wrap(pending->reason, 39, 0), width - 322, 120, 13,
                 {70, 55, 30, 255});
    surface.text("Plan " + wrap(pending->details.canonical_plan_hash, 32, 0),
                 width - 322, 144, 12, {70, 55, 30, 255});
    surface.text("Sandbox " +
                     wrap(pending->details.sandbox_plan.dump(), 32, 0),
                 width - 322, 168, 12, {70, 55, 30, 255});
    surface.text(wrap(pending->arguments.dump(), 39, 0), width - 322, 194, 13,
                 {70, 55, 30, 255});
  }
}

std::shared_ptr<snow::ModelProvider> App::create_model() const {
  const auto layout =
      snow::ConfigLayout::resolve(config_.workspace, config_.config_dir_name);
  const auto document = snow::load_providers_config(layout);
  const auto provider = document.value("default", tokmon::Json::object());
  const auto endpoint = provider.value("endpoint", "");
  if (!endpoint.empty()) {
    std::string api_key;
    if (const auto value = tokmon::environment_variable(
            provider.value("api_key_env", "")))
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
