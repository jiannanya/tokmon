#pragma once

#include <snow/config.hpp>
#include <snow/artifact.hpp>
#include <snow/context.hpp>
#include <snow/model.hpp>
#include <snow/replay.hpp>
#include <snow/surface.hpp>
#include <snow/tools.hpp>
#include <snow/trajectory.hpp>

#include <arche/types.hpp>

#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>

namespace snow {

enum class TurnEndReason {
  completed,
  aborted,
  blocked,
  error,
  max_tokens,
  interrupted,
};

[[nodiscard]] std::string_view to_string(TurnEndReason reason) noexcept;

struct RunOptions {
  std::string model;
  tokmon::Json model_parameters{tokmon::Json::object()};
  tokmon::Json attachments{tokmon::Json::array()};
  std::size_t max_steps{32};
};

struct RunResult {
  tokmon::RunId run_id;
  tokmon::TurnId turn_id;
  TurnEndReason reason{TurnEndReason::error};
  std::string final_text;
  std::uint64_t last_seq{0};
};

class Agent final {
public:
  Agent(std::shared_ptr<TrajectoryJournal> journal,
        std::shared_ptr<ModelProvider> model,
        std::shared_ptr<ToolRegistry> tools,
        std::shared_ptr<PolicyEngine> policy,
        std::shared_ptr<ApprovalService> approvals,
        BootstrapConfig config,
        std::function<arche::CompositionEpoch()> epoch,
        arche::FiberId producer_fiber = arche::FiberId("snow.loop.direct"),
        std::shared_ptr<RawTraceVault> raw_vault = {});

  tokmon::SessionId create_session(tokmon::Json metadata = tokmon::Json::object());
  tokmon::SessionId fork_session(
      const tokmon::SessionId& parent,
      tokmon::Json metadata = tokmon::Json::object(),
      std::uint64_t seed_seq = 0);
  void close_session(const tokmon::SessionId& session);
  std::uint64_t steer(const tokmon::SessionId& session, std::string message);
  [[nodiscard]] bool has_active_runs() const;
  RunResult run(const tokmon::SessionId& session, std::string user_message,
                RunOptions options = {}, std::stop_token stop = {});
  [[nodiscard]] std::vector<TrajectoryEvent> events(
      const tokmon::SessionId& session, std::uint64_t after = 0) const;
  [[nodiscard]] std::vector<SessionSummary> sessions(
      std::size_t limit = 100) const;
  [[nodiscard]] tokmon::Json transcript(
      const tokmon::SessionId& session) const;
  [[nodiscard]] ReplayReport replay(
      const tokmon::SessionId& session,
      ReplayLevel level = ReplayLevel::control) const;

private:
  struct PreparedTool;
  TrajectoryEvent base_event(const std::string& type,
                             const tokmon::SessionId& session,
                             const tokmon::TraceId& trace,
                             const tokmon::RunId& run,
                             const tokmon::TurnId& turn) const;
  TrajectoryEvent append_event(TrajectoryEvent event);
  void redact(tokmon::Json& value) const;
  std::vector<ToolResult> execute_tools(
      const tokmon::SessionId& session, const tokmon::TraceId& trace,
      const tokmon::RunId& run, const tokmon::TurnId& turn,
      const tokmon::StepId& step, const std::vector<ToolCall>& calls,
      std::stop_token stop);

  std::shared_ptr<TrajectoryJournal> journal_;
  std::shared_ptr<ModelProvider> model_;
  std::shared_ptr<ToolRegistry> tools_;
  std::shared_ptr<PolicyEngine> policy_;
  std::shared_ptr<ApprovalService> approvals_;
  BootstrapConfig config_;
  ConfigLayout layout_;
  SurfaceProjection surface_;
  ContextSources context_sources_;
  std::function<arche::CompositionEpoch()> epoch_;
  arche::FiberId producer_fiber_;
  std::shared_ptr<RawTraceVault> raw_vault_;
  struct ActiveRun {
    tokmon::TraceId trace;
    tokmon::RunId run;
    tokmon::TurnId turn;
  };
  mutable std::mutex active_mutex_;
  std::map<std::string, ActiveRun, std::less<>> active_runs_;
};

} // namespace snow
