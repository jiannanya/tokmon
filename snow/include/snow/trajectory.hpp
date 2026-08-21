#pragma once

#include <arche/types.hpp>
#include <axon/signal.hpp>
#include <tokmon/common/types.hpp>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace snow {

class ProcessFileLock;

struct TrajectoryEvent {
  std::string type;
  std::uint32_t schema{1};
  std::uint64_t seq{0};
  std::string time;

  tokmon::TraceId trace_id;
  tokmon::SessionId session_id;
  std::optional<tokmon::RunId> run_id;
  std::optional<tokmon::TurnId> turn_id;
  std::optional<tokmon::StepId> step_id;
  std::optional<tokmon::ModelCallId> model_call_id;
  std::optional<tokmon::ToolCallId> tool_call_id;
  std::optional<tokmon::SpanId> span_id;
  std::optional<tokmon::SpanId> parent_span_id;

  arche::FiberId producer_fiber;
  arche::CompositionEpoch composition_epoch{0};
  std::vector<std::uint64_t> source_event_seqs;
  bool ignorable{false};
  tokmon::Json data{tokmon::Json::object()};
};

struct SessionSummary {
  tokmon::SessionId id;
  std::optional<tokmon::SessionId> parent_id;
  std::string created_at;
  std::optional<std::string> closed_at;
  tokmon::Json header{tokmon::Json::object()};
  std::uint64_t last_seq{0};
};

void to_json(tokmon::Json& out, const TrajectoryEvent& event);
void from_json(const tokmon::Json& in, TrajectoryEvent& event);
[[nodiscard]] std::string session_title_from_prompt(
    std::string_view prompt, std::size_t max_codepoints = 36);

class TrajectoryJournal final {
public:
  explicit TrajectoryJournal(std::filesystem::path database);
  ~TrajectoryJournal();
  TrajectoryJournal(const TrajectoryJournal&) = delete;
  TrajectoryJournal& operator=(const TrajectoryJournal&) = delete;

  tokmon::SessionId create_session(tokmon::Json header,
                                   std::optional<tokmon::SessionId> parent = {});
  tokmon::SessionId fork_session(const tokmon::SessionId& parent,
                                 tokmon::Json header = tokmon::Json::object(),
                                 std::uint64_t seed_seq = 0);
  TrajectoryEvent append(TrajectoryEvent event);
  std::vector<TrajectoryEvent> append_batch(
      std::vector<TrajectoryEvent> events);
  [[nodiscard]] std::vector<TrajectoryEvent> events(
      const tokmon::SessionId& session, std::uint64_t after_seq = 0,
      std::size_t limit = 10000) const;
  [[nodiscard]] tokmon::Json session_header(
      const tokmon::SessionId& session) const;
  bool set_session_title_from_prompt(const tokmon::SessionId& session,
                                     std::string_view prompt);
  // Forces the session title, unlike set_session_title_from_prompt which only
  // fills an empty one. Used by the desktop UI rename affordance.
  bool set_session_title(const tokmon::SessionId& session,
                         std::string_view title);
  [[nodiscard]] std::uint64_t last_seq(
      const tokmon::SessionId& session) const;
  [[nodiscard]] bool session_exists(const tokmon::SessionId& session) const;
  [[nodiscard]] std::vector<SessionSummary> sessions(
      std::size_t limit = 100) const;
  void repair_interrupted_sessions();
  void close_session(const tokmon::SessionId& session);
  [[nodiscard]] axon::Signal<TrajectoryEvent>& committed() noexcept {
    return committed_;
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return database_;
  }

private:
  void initialize();
  void execute(std::string_view sql) const;
  [[nodiscard]] TrajectoryEvent append_locked(TrajectoryEvent event);

  std::filesystem::path database_;
  std::unique_ptr<ProcessFileLock> writer_lock_;
  sqlite3* database_handle_{nullptr};
  mutable std::recursive_mutex mutex_;
  axon::Signal<TrajectoryEvent> committed_;
};

} // namespace snow
