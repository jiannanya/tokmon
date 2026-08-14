#include <snow/trajectory.hpp>

#include <sqlite3.h>

#include <tokmon/common/files.hpp>
#include <tokmon/common/digest.hpp>

#include <map>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace snow {
namespace {

class Statement final {
public:
  Statement(sqlite3* database, const char* sql) {
    if (sqlite3_prepare_v2(database, sql, -1, &value_, nullptr) != SQLITE_OK) {
      throw tokmon::Error("snow.sqlite.prepare", sqlite3_errmsg(database));
    }
  }
  ~Statement() {
    if (value_) {
      sqlite3_finalize(value_);
    }
  }
  sqlite3_stmt* get() const noexcept { return value_; }

private:
  sqlite3_stmt* value_{};
};

void bind_text(sqlite3_stmt* statement, int index, std::string_view value) {
  if (sqlite3_bind_text(statement, index, value.data(),
                        static_cast<int>(value.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    throw tokmon::Error("snow.sqlite.bind", "failed to bind text");
  }
}

std::optional<std::string> optional_id(const auto& value) {
  if (value) {
    return value->str();
  }
  return std::nullopt;
}

void put_optional(tokmon::Json& out, std::string_view key, const auto& value) {
  if (value) {
    out[std::string(key)] = value->str();
  }
}

std::string checksum(std::string_view value) {
  return tokmon::sha256_hex(value);
}

std::string legacy_checksum(std::string_view value) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto byte : value) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ULL;
  }
  constexpr char digits[] = "0123456789abcdef";
  std::string result(16, '0');
  for (int index = 15; index >= 0; --index) {
    result[static_cast<std::size_t>(index)] = digits[hash & 0x0fU];
    hash >>= 4U;
  }
  return result;
}

} // namespace

class ProcessFileLock final {
public:
  explicit ProcessFileLock(std::filesystem::path path) : path_(std::move(path)) {
#ifdef _WIN32
    handle_ = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      handle_ = nullptr;
      throw tokmon::Error("snow.writer.locked",
                          "Snow data root already has a writer",
                          {{"lock", path_.generic_string()},
                           {"windows_error", GetLastError()}});
    }
#else
    descriptor_ = open(path_.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor_ < 0 || flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
      if (descriptor_ >= 0) close(descriptor_);
      descriptor_ = -1;
      throw tokmon::Error("snow.writer.locked",
                          "Snow data root already has a writer",
                          {{"lock", path_.generic_string()}});
    }
#endif
  }
  ~ProcessFileLock() {
#ifdef _WIN32
    if (handle_) CloseHandle(handle_);
#else
    if (descriptor_ >= 0) {
      flock(descriptor_, LOCK_UN);
      close(descriptor_);
    }
#endif
  }

private:
  std::filesystem::path path_;
#ifdef _WIN32
  HANDLE handle_{nullptr};
#else
  int descriptor_{-1};
#endif
};

void to_json(tokmon::Json& out, const TrajectoryEvent& event) {
  out = {{"type", event.type},
         {"schema", event.schema},
         {"seq", event.seq},
         {"time", event.time},
         {"trace_id", event.trace_id.str()},
         {"session_id", event.session_id.str()},
         {"producer_fiber", event.producer_fiber.str()},
         {"composition_epoch", event.composition_epoch},
         {"source_event_seqs", event.source_event_seqs},
         {"ignorable", event.ignorable},
         {"data", event.data}};
  put_optional(out, "run_id", event.run_id);
  put_optional(out, "turn_id", event.turn_id);
  put_optional(out, "step_id", event.step_id);
  put_optional(out, "model_call_id", event.model_call_id);
  put_optional(out, "tool_call_id", event.tool_call_id);
  put_optional(out, "span_id", event.span_id);
  put_optional(out, "parent_span_id", event.parent_span_id);
}

void from_json(const tokmon::Json& in, TrajectoryEvent& event) {
  in.at("type").get_to(event.type);
  event.schema = in.value("schema", 1U);
  event.seq = in.value("seq", 0ULL);
  event.time = in.value("time", "");
  event.trace_id = tokmon::TraceId(in.value("trace_id", ""));
  event.session_id = tokmon::SessionId(in.value("session_id", ""));
  event.producer_fiber =
      arche::FiberId(in.value("producer_fiber", ""));
  event.composition_epoch = in.value("composition_epoch", 0ULL);
  event.source_event_seqs =
      in.value("source_event_seqs", std::vector<std::uint64_t>{});
  event.ignorable = in.value("ignorable", false);
  event.data = in.value("data", tokmon::Json::object());
  if (in.contains("run_id"))
    event.run_id = tokmon::RunId(in["run_id"].get<std::string>());
  if (in.contains("turn_id"))
    event.turn_id = tokmon::TurnId(in["turn_id"].get<std::string>());
  if (in.contains("step_id"))
    event.step_id = tokmon::StepId(in["step_id"].get<std::string>());
  if (in.contains("model_call_id"))
    event.model_call_id =
        tokmon::ModelCallId(in["model_call_id"].get<std::string>());
  if (in.contains("tool_call_id"))
    event.tool_call_id =
        tokmon::ToolCallId(in["tool_call_id"].get<std::string>());
  if (in.contains("span_id"))
    event.span_id = tokmon::SpanId(in["span_id"].get<std::string>());
  if (in.contains("parent_span_id"))
    event.parent_span_id =
        tokmon::SpanId(in["parent_span_id"].get<std::string>());
}

TrajectoryJournal::TrajectoryJournal(std::filesystem::path database)
    : database_(std::move(database)) {
  std::filesystem::create_directories(database_.parent_path());
  auto lock_path = database_;
  lock_path += ".writer.lock";
  writer_lock_ = std::make_unique<ProcessFileLock>(std::move(lock_path));
  if (sqlite3_open_v2(database_.string().c_str(), &database_handle_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    const std::string message =
        database_handle_ ? sqlite3_errmsg(database_handle_)
                         : "failed to allocate sqlite handle";
    if (database_handle_) {
      sqlite3_close(database_handle_);
      database_handle_ = nullptr;
    }
    throw tokmon::Error("snow.sqlite.open", message);
  }
  initialize();
}

TrajectoryJournal::~TrajectoryJournal() {
  if (database_handle_) {
    sqlite3_close(database_handle_);
  }
}

void TrajectoryJournal::initialize() {
  execute("PRAGMA journal_mode=WAL");
  execute("PRAGMA synchronous=FULL");
  execute("PRAGMA foreign_keys=ON");
  auto scalar = [&](const char* sql) {
    Statement statement(database_handle_, sql);
    return sqlite3_step(statement.get()) == SQLITE_ROW
               ? sqlite3_column_int(statement.get(), 0)
               : 0;
  };
  const auto old_version = scalar("PRAGMA user_version");
  const bool legacy_database =
      old_version == 0 &&
      scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
             "name='trajectory_events'") != 0;
  constexpr int current_version = 2;
  if (old_version > current_version) {
    throw tokmon::Error("snow.sqlite.version",
                        "database schema is newer than this Snow runtime",
                        {{"database_version", old_version},
                         {"runtime_version", current_version}});
  }
  if (legacy_database || (old_version > 0 && old_version < current_version)) {
    auto backup_path = database_;
    backup_path += ".backup-v" + std::to_string(old_version) + "-" +
                   std::to_string(tokmon::unix_millis()) + ".db";
    sqlite3* backup_database = nullptr;
    if (sqlite3_open_v2(backup_path.string().c_str(), &backup_database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK) {
      if (backup_database) sqlite3_close(backup_database);
      throw tokmon::Error("snow.sqlite.backup",
                          "failed to open migration backup");
    }
    auto* backup = sqlite3_backup_init(backup_database, "main",
                                       database_handle_, "main");
    const auto copied = backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE;
    if (backup) sqlite3_backup_finish(backup);
    sqlite3_close(backup_database);
    if (!copied)
      throw tokmon::Error("snow.sqlite.backup",
                          "failed to create migration backup");
  }
  execute("BEGIN IMMEDIATE");
  try {
    execute(R"SQL(
    CREATE TABLE IF NOT EXISTS sessions (
      id TEXT PRIMARY KEY,
      parent_id TEXT,
      created_at TEXT NOT NULL,
      closed_at TEXT,
      header_json TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS trajectory_events (
      session_id TEXT NOT NULL,
      seq INTEGER NOT NULL,
      type TEXT NOT NULL,
      time TEXT NOT NULL,
      envelope_json TEXT NOT NULL,
      checksum TEXT NOT NULL,
      PRIMARY KEY(session_id, seq),
      FOREIGN KEY(session_id) REFERENCES sessions(id)
    );
    CREATE INDEX IF NOT EXISTS trajectory_type_idx
      ON trajectory_events(session_id, type, seq);
    CREATE TABLE IF NOT EXISTS surface_checkpoints (
      session_id TEXT NOT NULL,
      seq INTEGER NOT NULL,
      projection_version INTEGER NOT NULL,
      data_json TEXT NOT NULL,
      PRIMARY KEY(session_id, seq)
    );
    CREATE TABLE IF NOT EXISTS artifacts (
      hash TEXT PRIMARY KEY,
      size INTEGER NOT NULL,
      media_type TEXT NOT NULL,
      path TEXT NOT NULL,
      created_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS tool_executions (
      idempotency_key TEXT PRIMARY KEY,
      session_id TEXT NOT NULL,
      tool_call_id TEXT NOT NULL,
      plan_hash TEXT NOT NULL,
      phase TEXT NOT NULL,
      result_json TEXT,
      updated_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS composition_epochs (
      epoch INTEGER PRIMARY KEY,
      composition_id TEXT NOT NULL,
      lock_json TEXT NOT NULL,
      committed_at TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS schema_migrations (
      version INTEGER PRIMARY KEY,
      applied_at TEXT NOT NULL
    );
    )SQL");
    execute(
        "INSERT OR IGNORE INTO schema_migrations(version,applied_at) VALUES(" +
        std::to_string(current_version) + ",datetime('now'))");
    execute("PRAGMA user_version=" + std::to_string(current_version));
    execute("COMMIT");
  } catch (...) {
    try {
      execute("ROLLBACK");
    } catch (...) {
    }
    throw;
  }
}

void TrajectoryJournal::execute(std::string_view sql) const {
  char* error = nullptr;
  const std::string owned(sql);
  if (sqlite3_exec(database_handle_, owned.c_str(), nullptr, nullptr, &error) !=
      SQLITE_OK) {
    std::string message = error ? error : "sqlite error";
    sqlite3_free(error);
    throw tokmon::Error("snow.sqlite.exec", std::move(message));
  }
}

tokmon::SessionId TrajectoryJournal::create_session(
    tokmon::Json header, std::optional<tokmon::SessionId> parent) {
  const tokmon::SessionId id(tokmon::make_uuid());
  const auto now = tokmon::iso8601();
  header["id"] = id.str();
  header["created_at"] = now;
  if (parent) {
    header["parent"] = parent->str();
    if (!header.contains("seed_seq")) header["seed_seq"] = last_seq(*parent);
  }

  TrajectoryEvent event;
  event.type = "session/header";
  event.session_id = id;
  event.trace_id = tokmon::TraceId(tokmon::make_uuid());
  event.time = now;
  event.producer_fiber = arche::FiberId("snow.session.trajectory");
  event.data = header;

  TrajectoryEvent seed;
  seed.type = "session/end-seed";
  seed.session_id = id;
  seed.trace_id = tokmon::TraceId(tokmon::make_uuid());
  seed.producer_fiber = arche::FiberId("snow.session.trajectory");
  seed.data = {{"parent", parent ? tokmon::Json(parent->str())
                                 : tokmon::Json(nullptr)},
               {"seed_seq", header.value("seed_seq", 0ULL)}};
  std::vector<TrajectoryEvent> committed;
  {
    std::lock_guard lock(mutex_);
    execute("BEGIN IMMEDIATE");
    try {
      Statement statement(
          database_handle_,
          "INSERT INTO sessions(id,parent_id,created_at,header_json)"
          " VALUES(?,?,?,?)");
      bind_text(statement.get(), 1, id.str());
      if (parent) {
        bind_text(statement.get(), 2, parent->str());
      } else {
        sqlite3_bind_null(statement.get(), 2);
      }
      bind_text(statement.get(), 3, now);
      const auto encoded = header.dump();
      bind_text(statement.get(), 4, encoded);
      if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw tokmon::Error("snow.session.create",
                            sqlite3_errmsg(database_handle_));
      }
      committed.push_back(append_locked(std::move(event)));
      committed.push_back(append_locked(std::move(seed)));
      execute("COMMIT");
    } catch (...) {
      execute("ROLLBACK");
      throw;
    }
  }
  for (const auto& value : committed) {
    committed_.emit(value);
  }
  return id;
}

tokmon::SessionId TrajectoryJournal::fork_session(
    const tokmon::SessionId& parent, tokmon::Json header,
    std::uint64_t seed_seq) {
  if (!session_exists(parent)) {
    throw tokmon::Error("snow.session.parent", "parent session does not exist");
  }
  const auto parent_last = last_seq(parent);
  if (seed_seq == 0) seed_seq = parent_last;
  if (seed_seq > parent_last) {
    throw tokmon::Error("snow.session.seed", "fork seed exceeds parent history");
  }
  header["seed_seq"] = seed_seq;
  header["origin"] = header.value("origin", "fork");
  return create_session(std::move(header), parent);
}

TrajectoryEvent TrajectoryJournal::append(TrajectoryEvent event) {
  TrajectoryEvent committed;
  {
    std::lock_guard lock(mutex_);
    committed = append_locked(std::move(event));
  }
  committed_.emit(committed);
  return committed;
}

std::vector<TrajectoryEvent> TrajectoryJournal::append_batch(
    std::vector<TrajectoryEvent> events) {
  {
    std::lock_guard lock(mutex_);
    execute("BEGIN IMMEDIATE");
    try {
      for (auto& event : events) {
        event = append_locked(std::move(event));
      }
      execute("COMMIT");
    } catch (...) {
      execute("ROLLBACK");
      throw;
    }
  }
  for (const auto& event : events) {
    committed_.emit(event);
  }
  return events;
}

TrajectoryEvent TrajectoryJournal::append_locked(TrajectoryEvent event) {
  if (event.session_id.empty()) {
    throw tokmon::Error("snow.event.session", "event requires session_id");
  }
  if (event.trace_id.empty()) {
    event.trace_id = tokmon::TraceId(tokmon::make_uuid());
  }
  if (event.time.empty()) {
    event.time = tokmon::iso8601();
  }
  event.seq = last_seq(event.session_id) + 1;
  const auto encoded = tokmon::Json(event).dump();
  Statement statement(
      database_handle_,
      "INSERT INTO trajectory_events(session_id,seq,type,time,envelope_json,"
      "checksum) VALUES(?,?,?,?,?,?)");
  bind_text(statement.get(), 1, event.session_id.str());
  sqlite3_bind_int64(statement.get(), 2,
                     static_cast<sqlite3_int64>(event.seq));
  bind_text(statement.get(), 3, event.type);
  bind_text(statement.get(), 4, event.time);
  bind_text(statement.get(), 5, encoded);
  const auto digest = checksum(encoded);
  bind_text(statement.get(), 6, digest);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    throw tokmon::Error("snow.event.append",
                        sqlite3_errmsg(database_handle_));
  }
  return event;
}

std::vector<TrajectoryEvent> TrajectoryJournal::events(
    const tokmon::SessionId& session, std::uint64_t after_seq,
    std::size_t limit) const {
  std::lock_guard lock(mutex_);
  Statement statement(
      database_handle_,
      "SELECT envelope_json,checksum FROM trajectory_events "
      "WHERE session_id=? AND seq>? ORDER BY seq LIMIT ?");
  bind_text(statement.get(), 1, session.str());
  sqlite3_bind_int64(statement.get(), 2,
                     static_cast<sqlite3_int64>(after_seq));
  sqlite3_bind_int64(statement.get(), 3,
                     static_cast<sqlite3_int64>(limit));
  std::vector<TrajectoryEvent> result;
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    const auto* text =
        reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
    const auto* digest =
        reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
    const std::string encoded = text ? text : "";
    const std::string stored = digest ? digest : "";
    if (checksum(encoded) != stored && legacy_checksum(encoded) != stored) {
      throw tokmon::Error("snow.event.checksum",
                          "trajectory checksum mismatch");
    }
    result.push_back(tokmon::Json::parse(encoded).get<TrajectoryEvent>());
  }
  return result;
}

tokmon::Json TrajectoryJournal::session_header(
    const tokmon::SessionId& session) const {
  std::lock_guard lock(mutex_);
  Statement statement(database_handle_,
                      "SELECT header_json FROM sessions WHERE id=?");
  bind_text(statement.get(), 1, session.str());
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw tokmon::Error("snow.session.missing",
                        "session not found: " + session.str());
  }
  const auto* text =
      reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
  return tokmon::Json::parse(text ? text : "{}");
}

std::uint64_t TrajectoryJournal::last_seq(
    const tokmon::SessionId& session) const {
  std::lock_guard lock(mutex_);
  Statement statement(
      database_handle_,
      "SELECT COALESCE(MAX(seq),0) FROM trajectory_events WHERE session_id=?");
  bind_text(statement.get(), 1, session.str());
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return 0;
  }
  return static_cast<std::uint64_t>(
      sqlite3_column_int64(statement.get(), 0));
}

bool TrajectoryJournal::session_exists(
    const tokmon::SessionId& session) const {
  std::lock_guard lock(mutex_);
  Statement statement(database_handle_,
                      "SELECT 1 FROM sessions WHERE id=? LIMIT 1");
  bind_text(statement.get(), 1, session.str());
  return sqlite3_step(statement.get()) == SQLITE_ROW;
}

std::vector<SessionSummary> TrajectoryJournal::sessions(
    std::size_t limit) const {
  std::lock_guard lock(mutex_);
  Statement statement(database_handle_, R"SQL(
    SELECT s.id, s.parent_id, s.created_at, s.closed_at, s.header_json,
           COALESCE(MAX(e.seq), 0)
      FROM sessions s
      LEFT JOIN trajectory_events e ON e.session_id = s.id
     GROUP BY s.id, s.parent_id, s.created_at, s.closed_at, s.header_json
     ORDER BY s.created_at DESC
     LIMIT ?
  )SQL");
  sqlite3_bind_int64(statement.get(), 1,
                     static_cast<sqlite3_int64>(std::clamp<std::size_t>(
                         limit, std::size_t{1}, std::size_t{1000})));
  std::vector<SessionSummary> result;
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    const auto text = [&](int column) -> std::string {
      const auto* value = sqlite3_column_text(statement.get(), column);
      return value ? reinterpret_cast<const char*>(value) : std::string{};
    };
    SessionSummary item;
    item.id = tokmon::SessionId(text(0));
    if (sqlite3_column_type(statement.get(), 1) != SQLITE_NULL)
      item.parent_id = tokmon::SessionId(text(1));
    item.created_at = text(2);
    if (sqlite3_column_type(statement.get(), 3) != SQLITE_NULL)
      item.closed_at = text(3);
    item.header = tokmon::Json::parse(text(4));
    item.last_seq = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement.get(), 5));
    result.push_back(std::move(item));
  }
  return result;
}

void TrajectoryJournal::repair_interrupted_sessions() {
  std::vector<tokmon::SessionId> ids;
  {
    std::lock_guard lock(mutex_);
    Statement sessions(database_handle_,
                       "SELECT id FROM sessions WHERE closed_at IS NULL");
    while (sqlite3_step(sessions.get()) == SQLITE_ROW) {
      const auto* value = reinterpret_cast<const char*>(
          sqlite3_column_text(sessions.get(), 0));
      ids.emplace_back(value ? value : "");
    }
  }
  std::vector<TrajectoryEvent> committed;
  for (const auto& id : ids) {
    const auto history = events(id);
    std::map<std::string, TrajectoryEvent, std::less<>> open_runs;
    std::map<std::string, TrajectoryEvent, std::less<>> open_turns;
    std::map<std::string, TrajectoryEvent, std::less<>> open_steps;
    std::map<std::string, TrajectoryEvent, std::less<>> dispatched;
    for (const auto& event : history) {
      if (event.type == "run/start" && event.run_id) {
        open_runs[event.run_id->str()] = event;
      } else if (event.type == "run/end" && event.run_id) {
        open_runs.erase(event.run_id->str());
      } else if (event.type == "turn/start" && event.turn_id) {
        open_turns[event.turn_id->str()] = event;
      } else if (event.type == "turn/end" && event.turn_id) {
        open_turns.erase(event.turn_id->str());
      } else if (event.type == "step/start" && event.step_id) {
        open_steps[event.step_id->str()] = event;
      } else if (event.type == "step/end" && event.step_id) {
        open_steps.erase(event.step_id->str());
      } else if (event.type == "tool/dispatch" && event.tool_call_id) {
        dispatched[event.tool_call_id->str()] = event;
      } else if (event.type == "tool/result" && event.tool_call_id) {
        dispatched.erase(event.tool_call_id->str());
      }
    }
    if (open_runs.empty() && open_turns.empty() && open_steps.empty() &&
        dispatched.empty()) {
      continue;
    }

    std::vector<TrajectoryEvent> repairs;
    auto recovery = history.back();
    recovery.seq = 0;
    recovery.time.clear();
    recovery.type = "recovery/crash-repair";
    recovery.source_event_seqs = {history.back().seq};
    recovery.data = {{"repaired_after_seq", history.back().seq},
                     {"open_runs", open_runs.size()},
                     {"open_turns", open_turns.size()},
                     {"open_steps", open_steps.size()},
                     {"outcomes_unknown", dispatched.size()}};
    repairs.push_back(std::move(recovery));
    for (const auto& [tool_id, dispatch] : dispatched) {
      auto result = dispatch;
      result.seq = 0;
      result.time.clear();
      result.type = "tool/result";
      result.source_event_seqs = {dispatch.seq};
      result.data = {{"tool_call_id", tool_id},
                     {"name", dispatch.data.value("name", "unknown")},
                     {"success", false},
                     {"status", "outcome_unknown"},
                     {"content",
                      "process stopped after durable dispatch but before a "
                      "result was committed; automatic retry is forbidden"},
                     {"canonical_plan_hash",
                      dispatch.data.value("canonical_plan_hash", "")},
                     {"idempotency_key",
                      dispatch.data.value("idempotency_key", "")}};
      repairs.push_back(std::move(result));
    }
    for (const auto& [_, start] : open_steps) {
      auto end = start;
      end.seq = 0;
      end.time.clear();
      end.type = "step/end";
      end.source_event_seqs = {start.seq};
      end.data = {{"reason", "interrupted"}, {"cause", "crash_repair"}};
      repairs.push_back(std::move(end));
    }
    for (const auto& [_, start] : open_turns) {
      auto end = start;
      end.seq = 0;
      end.time.clear();
      end.type = "turn/end";
      end.source_event_seqs = {start.seq};
      end.data = {{"reason", "interrupted"},
                  {"cause", "crash_repair"},
                  {"repaired_after_seq", history.back().seq}};
      repairs.push_back(std::move(end));
    }
    for (const auto& [_, start] : open_runs) {
      auto end = start;
      end.seq = 0;
      end.time.clear();
      end.type = "run/end";
      end.source_event_seqs = {start.seq};
      end.data = {{"reason", "interrupted"}, {"cause", "crash_repair"}};
      repairs.push_back(std::move(end));
    }
    {
      std::lock_guard lock(mutex_);
      execute("BEGIN IMMEDIATE");
      try {
        for (auto& repair : repairs) {
          committed.push_back(append_locked(std::move(repair)));
        }
        execute("COMMIT");
      } catch (...) {
        execute("ROLLBACK");
        throw;
      }
    }
  }
  for (const auto& event : committed) committed_.emit(event);
}

void TrajectoryJournal::close_session(const tokmon::SessionId& session) {
  TrajectoryEvent committed;
  {
    std::lock_guard lock(mutex_);
    execute("BEGIN IMMEDIATE");
    try {
      Statement statement(database_handle_,
                          "UPDATE sessions SET closed_at=? "
                          "WHERE id=? AND closed_at IS NULL");
      const auto now = tokmon::iso8601();
      bind_text(statement.get(), 1, now);
      bind_text(statement.get(), 2, session.str());
      if (sqlite3_step(statement.get()) != SQLITE_DONE ||
          sqlite3_changes(database_handle_) != 1) {
        throw tokmon::Error("snow.session.close",
                            "session is missing or already closed");
      }
      TrajectoryEvent event;
      event.type = "session/closed";
      event.session_id = session;
      event.trace_id = tokmon::TraceId(tokmon::make_uuid());
      event.producer_fiber = arche::FiberId("snow.session.trajectory");
      event.data = {{"closed_at", now}};
      committed = append_locked(std::move(event));
      execute("COMMIT");
    } catch (...) {
      execute("ROLLBACK");
      throw;
    }
  }
  committed_.emit(committed);
}

} // namespace snow
