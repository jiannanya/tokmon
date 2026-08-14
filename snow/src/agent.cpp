#include <snow/agent.hpp>

#include <tokmon/common/digest.hpp>

#include <algorithm>
#include <chrono>
#include <future>

namespace snow {

std::string_view to_string(TurnEndReason reason) noexcept {
  switch (reason) {
  case TurnEndReason::completed:
    return "completed";
  case TurnEndReason::aborted:
    return "aborted";
  case TurnEndReason::blocked:
    return "blocked";
  case TurnEndReason::error:
    return "error";
  case TurnEndReason::max_tokens:
    return "max_tokens";
  case TurnEndReason::interrupted:
    return "interrupted";
  }
  return "error";
}

Agent::Agent(std::shared_ptr<TrajectoryJournal> journal,
             std::shared_ptr<ModelProvider> model,
             std::shared_ptr<ToolRegistry> tools,
             std::shared_ptr<PolicyEngine> policy,
             std::shared_ptr<ApprovalService> approvals,
             BootstrapConfig config,
             std::function<arche::CompositionEpoch()> epoch,
             arche::FiberId producer_fiber,
             std::shared_ptr<RawTraceVault> raw_vault)
    : journal_(std::move(journal)),
      model_(std::move(model)),
      tools_(std::move(tools)),
      policy_(std::move(policy)),
      approvals_(std::move(approvals)),
      config_(std::move(config)),
      layout_(ConfigLayout::resolve(config_.workspace,
                                    config_.config_dir_name)),
      context_sources_(layout_),
      epoch_(std::move(epoch)),
      producer_fiber_(std::move(producer_fiber)),
      raw_vault_(std::move(raw_vault)) {}

struct Agent::PreparedTool {
  ToolCall call;
  std::shared_ptr<Tool> tool;
  std::optional<CanonicalToolPlan> plan;
  ToolResult result;
  bool executable{false};
};

tokmon::SessionId Agent::create_session(tokmon::Json metadata) {
  metadata["cwd"] = layout_.workspace.generic_string();
  metadata["config_dir_name"] = layout_.config_dir_name;
  metadata["origin"] = "snow";
  metadata["composition_epoch"] = epoch_();
  return journal_->create_session(std::move(metadata));
}

tokmon::SessionId Agent::fork_session(const tokmon::SessionId& parent,
                                      tokmon::Json metadata,
                                      std::uint64_t seed_seq) {
  metadata["cwd"] = layout_.workspace.generic_string();
  metadata["config_dir_name"] = layout_.config_dir_name;
  metadata["composition_epoch"] = epoch_();
  return journal_->fork_session(parent, std::move(metadata), seed_seq);
}

void Agent::close_session(const tokmon::SessionId& session) {
  journal_->close_session(session);
}

std::uint64_t Agent::steer(const tokmon::SessionId& session,
                           std::string message) {
  ActiveRun active;
  {
    std::lock_guard lock(active_mutex_);
    const auto found = active_runs_.find(session.str());
    if (found == active_runs_.end())
      throw tokmon::Error("snow.turn.inactive",
                          "the session has no active turn");
    active = found->second;
  }
  auto event = base_event("user/steer", session, active.trace, active.run,
                          active.turn);
  event.data = {{"content", std::move(message)}, {"input_kind", "steer"}};
  return append_event(std::move(event)).seq;
}

bool Agent::has_active_runs() const {
  std::lock_guard lock(active_mutex_);
  return !active_runs_.empty();
}

RunResult Agent::run(const tokmon::SessionId& session,
                     std::string user_message, RunOptions options,
                     std::stop_token stop) {
  const auto turn_started_at = std::chrono::steady_clock::now();
  (void)journal_->session_header(session);
  if (options.max_steps == 0) {
    options.max_steps = config_.max_steps;
  }
  const tokmon::TraceId trace(tokmon::make_uuid());
  const tokmon::RunId run(tokmon::make_uuid());
  const tokmon::TurnId turn(tokmon::make_uuid());
  {
    std::lock_guard lock(active_mutex_);
    if (active_runs_.contains(session.str()))
      throw tokmon::Error("snow.turn.active",
                          "the session already has an active turn");
    active_runs_.emplace(session.str(), ActiveRun{trace, run, turn});
  }
  struct ClearActive {
    Agent* agent;
    std::string session;
    ~ClearActive() {
      std::lock_guard lock(agent->active_mutex_);
      agent->active_runs_.erase(session);
    }
  } clear_active{this, session.str()};

  auto append = [&](TrajectoryEvent event) {
    return append_event(std::move(event));
  };

  auto run_start = base_event("run/start", session, trace, run, turn);
  run_start.data = {{"workspace", layout_.workspace.generic_string()}};
  append(std::move(run_start));
  auto turn_start = base_event("turn/start", session, trace, run, turn);
  turn_start.data = {{"input_kind", "next-turn"}};
  append(std::move(turn_start));
  auto claimed = base_event("inbox/claimed", session, trace, run, turn);
  claimed.data = {{"kind", "next-turn"}};
  append(std::move(claimed));

  RunResult result{run, turn, TurnEndReason::error, "", 0};
  try {
    for (std::size_t step_index = 0; step_index < options.max_steps;
         ++step_index) {
      if (stop.stop_requested()) {
        result.reason = TurnEndReason::aborted;
        break;
      }
      const tokmon::StepId step(tokmon::make_uuid());
      auto step_start = base_event("step/start", session, trace, run, turn);
      step_start.step_id = step;
      step_start.data = {{"index", step_index}};
      append(std::move(step_start));

      if (step_index == 0) {
        auto user = base_event("user/message", session, trace, run, turn);
        user.step_id = step;
        user.data = {{"content", user_message},
                     {"attachments", options.attachments}};
        append(std::move(user));
      }

      const auto contributions = context_sources_.collect();
      const auto prompt = context_sources_.system_prompt(contributions);
      const auto context_provenance =
          context_sources_.provenance(contributions);
      for (const auto& contribution : contributions) {
        auto source =
            base_event("context/source", session, trace, run, turn);
        source.step_id = step;
        source.data = {{"kind", contribution.kind},
                       {"source", contribution.source},
                       {"hash", contribution.hash},
                       {"bytes", contribution.bytes},
                       {"sensitive", contribution.sensitive}};
        source.ignorable = true;
        append(std::move(source));
      }

      auto effective_history = journal_->events(session);
      ModelRequest request;
      request.model = options.model;
      request.messages = surface_.model_messages(effective_history);
      if (request.messages.dump().size() > config_.max_context_chars &&
          request.messages.size() > 4) {
        const auto replace_end = request.messages.size() / 2;
        std::string summary = "Earlier conversation compacted:\n";
        std::vector<std::uint64_t> sources;
        const auto projected = surface_.project(effective_history);
        for (std::size_t index = 0;
             index < replace_end && index < projected.size(); ++index) {
          summary += projected[index].role + ": " +
                     projected[index].content.dump().substr(0, 512) + "\n";
          sources.insert(sources.end(),
                         projected[index].source_event_seqs.begin(),
                         projected[index].source_event_seqs.end());
        }
        auto compaction =
            base_event("context/compaction", session, trace, run, turn);
        compaction.step_id = step;
        compaction.source_event_seqs = sources;
        compaction.data = {{"id", "compaction-" + step.str()},
                           {"start", 0},
                           {"end", replace_end},
                           {"summary", summary},
                           {"method", "deterministic-fallback"}};
        append(std::move(compaction));
        effective_history = journal_->events(session);
        request.messages = surface_.model_messages(effective_history);
      }
      const tokmon::Json system_message = {
          {"role", "system"}, {"content", prompt}};
      request.messages.insert(request.messages.begin(), system_message);
      request.tools = tools_->model_schema();
      request.parameters = options.model_parameters;
      const tokmon::ModelCallId model_call(tokmon::make_uuid());

      auto header = base_event("request/header", session, trace, run, turn);
      header.step_id = step;
      header.model_call_id = model_call;
      header.data = {
          {"provider", model_->id()},
          {"model", request.model},
          {"parameters", request.parameters},
          {"tools", request.tools},
          {"system_prompt_hash", "sha256:" + tokmon::sha256_hex(prompt)},
          {"context_sources", context_provenance},
          {"config_dir_name", layout_.config_dir_name},
          {"composition_epoch", epoch_()}};
      append(std::move(header));

      auto context = base_event("request/context", session, trace, run, turn);
      context.step_id = step;
      context.model_call_id = model_call;
      context.data = {{"messages", request.messages},
                      {"contributions", context_provenance},
                      {"source_last_seq", journal_->last_seq(session)}};
      append(std::move(context));

      auto requested = base_event("model/request", session, trace, run, turn);
      requested.step_id = step;
      requested.model_call_id = model_call;
      requested.data = {{"provider", model_->id()}};
      append(std::move(requested));

      auto response = model_->complete(
          request, [&](std::string_view kind, std::string_view chunk) {
            const bool raw = kind == "provider.raw";
            auto event = base_event(
                raw ? "model/raw-chunk"
                    : (kind == "reasoning" ? "assistant/reasoning-chunk"
                                            : "assistant/chunk"),
                session, trace, run, turn);
            event.step_id = step;
            event.model_call_id = model_call;
            event.data = raw
                             ? tokmon::Json{{"bytes", chunk.size()},
                                            {"sha256", tokmon::sha256_hex(chunk)},
                                            {"kind", kind}}
                             : tokmon::Json{{"content", chunk}, {"kind", kind}};
            if (raw_vault_ && raw_vault_->enabled() && !chunk.empty()) {
              event.data["raw_trace"] = raw_vault_->put_text(
                  raw ? "model.transport-chunk" : "model.semantic-chunk",
                  chunk);
            }
            event.ignorable = raw;
            append(std::move(event));
          }, stop);

      tokmon::Json calls = tokmon::Json::array();
      for (const auto& call : response.tool_calls) {
        calls.push_back({{"id", call.id.str()},
                         {"type", "function"},
                         {"function",
                          {{"name", call.name},
                           {"arguments", call.arguments.dump()}}}});
      }
      auto message =
          base_event("assistant/message", session, trace, run, turn);
      message.step_id = step;
      message.model_call_id = model_call;
      message.data = {
          {"content", response.content},
          {"reasoning", response.reasoning ? tokmon::Json(*response.reasoning)
                                            : tokmon::Json(nullptr)},
          {"tool_calls", calls},
          {"finish_reason", response.finish_reason},
          {"usage",
           {{"input_tokens", response.usage.input_tokens},
            {"output_tokens", response.usage.output_tokens},
            {"cached_tokens", response.usage.cached_tokens}}},
          {"provider_metadata", response.provider_metadata}};
      append(std::move(message));

      if (response.tool_calls.empty()) {
        auto step_end = base_event("step/end", session, trace, run, turn);
        step_end.step_id = step;
        step_end.data = {{"reason", "model-finished"}};
        append(std::move(step_end));
        result.reason = TurnEndReason::completed;
        result.final_text = response.content;
        break;
      }

      for (const auto& tool_result :
           execute_tools(session, trace, run, turn, step,
                         response.tool_calls, stop)) {
        if (tool_result.status == "blocked" ||
            tool_result.status == "denied") {
          result.reason = TurnEndReason::blocked;
        }
      }
      auto step_end = base_event("step/end", session, trace, run, turn);
      step_end.step_id = step;
      step_end.data = {{"reason", "tools-completed"}};
      append(std::move(step_end));
    }
    if (result.reason == TurnEndReason::error) {
      result.reason = stop.stop_requested() ? TurnEndReason::aborted
                                            : TurnEndReason::max_tokens;
    }
  } catch (const std::exception& error) {
    const bool cancelled = stop.stop_requested();
    auto failure = base_event(cancelled ? "agent/cancelled" : "agent/error",
                              session, trace, run, turn);
    failure.data = {{"message", error.what()}};
    append(std::move(failure));
    result.reason = cancelled ? TurnEndReason::aborted : TurnEndReason::error;
  }

  tokmon::Json safe_final = result.final_text;
  redact(safe_final);
  result.final_text = safe_final.get<std::string>();
  auto turn_end = base_event("turn/end", session, trace, run, turn);
  turn_end.data = {{"reason", to_string(result.reason)},
                   {"final_text", result.final_text},
                   {"elapsed_ms",
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - turn_started_at)
                        .count()}};
  append(std::move(turn_end));
  auto run_end = base_event("run/end", session, trace, run, turn);
  run_end.data = {{"reason", to_string(result.reason)}};
  append(std::move(run_end));
  result.last_seq = journal_->last_seq(session);
  return result;
}

std::vector<TrajectoryEvent> Agent::events(
    const tokmon::SessionId& session, std::uint64_t after) const {
  return journal_->events(session, after);
}

std::vector<SessionSummary> Agent::sessions(std::size_t limit) const {
  return journal_->sessions(limit);
}

tokmon::Json Agent::transcript(const tokmon::SessionId& session) const {
  tokmon::Json result = tokmon::Json::array();
  for (const auto& item : surface_.project(journal_->events(session))) {
    result.push_back(item);
  }
  return result;
}

ReplayReport Agent::replay(const tokmon::SessionId& session,
                           ReplayLevel level) const {
  return ReplayEngine{}.replay(journal_->events(session), level);
}

TrajectoryEvent Agent::base_event(const std::string& type,
                                  const tokmon::SessionId& session,
                                  const tokmon::TraceId& trace,
                                  const tokmon::RunId& run,
                                  const tokmon::TurnId& turn) const {
  TrajectoryEvent event;
  event.type = type;
  event.trace_id = trace;
  event.session_id = session;
  event.run_id = run;
  event.turn_id = turn;
  event.producer_fiber = producer_fiber_;
  event.composition_epoch = epoch_();
  return event;
}

TrajectoryEvent Agent::append_event(TrajectoryEvent event) {
  redact(event.data);
  return journal_->append(std::move(event));
}

void Agent::redact(tokmon::Json& value) const {
  if (value.is_string()) {
    auto text = value.get<std::string>();
    for (const auto& secret : config_.sensitive_values) {
      if (secret.size() < 4) continue;
      std::size_t offset = 0;
      while ((offset = text.find(secret, offset)) != std::string::npos) {
        text.replace(offset, secret.size(), "[REDACTED]");
        offset += 10;
      }
    }
    value = std::move(text);
    return;
  }
  if (value.is_array()) {
    for (auto& child : value) redact(child);
  } else if (value.is_object()) {
    for (auto& [_, child] : value.items()) redact(child);
  }
}

std::vector<ToolResult> Agent::execute_tools(
    const tokmon::SessionId& session, const tokmon::TraceId& trace,
    const tokmon::RunId& run, const tokmon::TurnId& turn,
    const tokmon::StepId& step, const std::vector<ToolCall>& calls,
    std::stop_token stop) {
  std::vector<PreparedTool> prepared;
  prepared.reserve(calls.size());
  for (const auto& call : calls) {
    PreparedTool item;
    item.call = call;
    auto proposed = base_event("tool/call", session, trace, run, turn);
    proposed.step_id = step;
    proposed.tool_call_id = call.id;
    proposed.data = {{"tool_call_id", call.id.str()},
                     {"name", call.name},
                     {"arguments", call.arguments}};
    append_event(std::move(proposed));

    item.tool = tools_->find(call.name);
    if (!item.tool) {
      item.result = {false, "error", "unknown tool: " + call.name};
      prepared.push_back(std::move(item));
      continue;
    }
    try {
      item.plan =
          normalize_tool_plan(item.tool->definition(), call.arguments, call.id);
    } catch (const std::exception& error) {
      item.result = {false, "invalid", error.what()};
      auto admission =
          base_event("tool/admission", session, trace, run, turn);
      admission.step_id = step;
      admission.tool_call_id = call.id;
      admission.data = {{"tool_call_id", call.id.str()},
                        {"accepted", false}, {"reason", error.what()}};
      append_event(std::move(admission));
      prepared.push_back(std::move(item));
      continue;
    }

    auto normalized = base_event("tool/normalized", session, trace, run, turn);
    normalized.step_id = step;
    normalized.tool_call_id = call.id;
    normalized.data = {{"tool_call_id", call.id.str()},
                       {"name", item.plan->tool},
                       {"canonical_arguments", item.plan->arguments},
                       {"canonical_plan_hash", item.plan->hash},
                       {"idempotency_key", item.plan->idempotency_key}};
    append_event(std::move(normalized));
    auto admission = base_event("tool/admission", session, trace, run, turn);
    admission.step_id = step;
    admission.tool_call_id = call.id;
    admission.data = {{"tool_call_id", call.id.str()}, {"accepted", true},
                      {"canonical_plan_hash", item.plan->hash}};
    append_event(std::move(admission));

    std::string policy_reason;
    const auto disposition = policy_->decide(
        item.tool->definition(), item.plan->arguments, &policy_reason);
    const auto disposition_name =
        disposition == ToolDisposition::allow
            ? "allow"
            : (disposition == ToolDisposition::ask ? "ask" : "deny");
    const tokmon::Json sandbox_plan = {
        {"filesystem",
         item.plan->read_only ? "workspace-read" : "workspace-write"},
        {"process", call.name == "shell" ? "job-contained" : "none"},
        {"network", call.name.starts_with("mcp__") ? "server-declared"
                                                     : "none"},
        {"strong_os_sandbox", false}};
    auto policy_event =
        base_event("tool/policy-decision", session, trace, run, turn);
    policy_event.step_id = step;
    policy_event.tool_call_id = call.id;
    policy_event.data = {{"tool_call_id", call.id.str()},
                         {"decision", disposition_name},
                         {"reason", policy_reason},
                         {"canonical_plan_hash", item.plan->hash}};
    append_event(std::move(policy_event));

    bool allowed = disposition == ToolDisposition::allow;
    if (disposition == ToolDisposition::ask) {
      auto request = base_event("approval/request", session, trace, run, turn);
      request.step_id = step;
      request.tool_call_id = call.id;
      request.data = {{"tool_call_id", call.id.str()},
                      {"name", call.name},
                      {"canonical_arguments", item.plan->arguments},
                      {"canonical_plan_hash", item.plan->hash},
                      {"idempotency_key", item.plan->idempotency_key},
                      {"sandbox_plan", sandbox_plan},
                      {"reason", policy_reason}};
      append_event(std::move(request));
      allowed = approvals_ && approvals_->approve(
                                  item.tool->definition(),
                                  item.plan->arguments, policy_reason,
                                  {.canonical_plan_hash = item.plan->hash,
                                   .idempotency_key =
                                       item.plan->idempotency_key,
                                   .sandbox_plan = sandbox_plan});
      const auto revalidated = normalize_tool_plan(
          item.tool->definition(), item.plan->arguments, call.id);
      if (revalidated.hash != item.plan->hash) {
        allowed = false;
        auto invalidated =
            base_event("approval/invalidated", session, trace, run, turn);
        invalidated.step_id = step;
        invalidated.tool_call_id = call.id;
        invalidated.data = {{"tool_call_id", call.id.str()},
                            {"approved_plan_hash", item.plan->hash},
                            {"current_plan_hash", revalidated.hash}};
        append_event(std::move(invalidated));
      }
      auto approval =
          base_event("approval/result", session, trace, run, turn);
      approval.step_id = step;
      approval.tool_call_id = call.id;
      approval.data = {{"tool_call_id", call.id.str()},
                       {"approved", allowed},
                       {"canonical_plan_hash", item.plan->hash}};
      append_event(std::move(approval));
    }

    auto sandbox = base_event("sandbox/plan", session, trace, run, turn);
    sandbox.step_id = step;
    sandbox.tool_call_id = call.id;
    sandbox.data = {
        {"tool_call_id", call.id.str()},
        {"canonical_plan_hash", item.plan->hash},
        {"filesystem", sandbox_plan["filesystem"]},
        {"process", sandbox_plan["process"]},
        {"network", sandbox_plan["network"]},
        {"strong_os_sandbox", sandbox_plan["strong_os_sandbox"]}};
    append_event(std::move(sandbox));

    if (!allowed) {
      item.result = {false,
                     disposition == ToolDisposition::deny ? "denied" : "blocked",
                     "tool execution was not approved"};
    } else if (stop.stop_requested()) {
      item.result = {false, "aborted", "turn was cancelled"};
    } else {
      item.executable = true;
    }
    prepared.push_back(std::move(item));
  }

  auto record_dispatch = [&](PreparedTool& item) {
    auto start = base_event("tool/start", session, trace, run, turn);
    start.step_id = step;
    start.tool_call_id = item.call.id;
    start.data = {{"tool_call_id", item.call.id.str()},
                  {"name", item.call.name},
                  {"canonical_plan_hash", item.plan->hash}};
    append_event(std::move(start));
    auto dispatch = base_event("tool/dispatch", session, trace, run, turn);
    dispatch.step_id = step;
    dispatch.tool_call_id = item.call.id;
    dispatch.data = {{"tool_call_id", item.call.id.str()},
                     {"name", item.call.name},
                     {"canonical_plan_hash", item.plan->hash},
                     {"idempotency_key", item.plan->idempotency_key}};
    append_event(std::move(dispatch));
  };
  auto execute = [&](PreparedTool& item) {
    try {
      return item.tool->execute(item.plan->arguments, stop);
    } catch (const std::exception& error) {
      return ToolResult{false, "error", error.what()};
    } catch (...) {
      return ToolResult{false, "error", "unknown tool failure"};
    }
  };

  for (std::size_t index = 0; index < prepared.size();) {
    if (!prepared[index].executable) {
      ++index;
      continue;
    }
    if (prepared[index].plan->parallel_safe) {
      const auto begin = index;
      while (index < prepared.size() && prepared[index].executable &&
             prepared[index].plan->parallel_safe) {
        record_dispatch(prepared[index]);
        ++index;
      }
      std::vector<std::future<ToolResult>> futures;
      futures.reserve(index - begin);
      for (auto position = begin; position < index; ++position) {
        futures.push_back(std::async(std::launch::async,
                                     [&, position] { return execute(prepared[position]); }));
      }
      for (std::size_t offset = 0; offset < futures.size(); ++offset) {
        prepared[begin + offset].result = futures[offset].get();
      }
    } else {
      record_dispatch(prepared[index]);
      prepared[index].result = execute(prepared[index]);
      ++index;
    }
  }

  std::vector<ToolResult> results;
  results.reserve(prepared.size());
  for (auto& item : prepared) {
    if (raw_vault_ && raw_vault_->enabled() && !item.result.content.empty()) {
      item.result.metadata["raw_trace"] =
          raw_vault_->put_text("tool.result", item.result.content);
    }
    if (item.result.content.size() > config_.max_tool_result_bytes) {
      item.result.content.resize(config_.max_tool_result_bytes);
      item.result.content += "\n...[truncated by Snow]";
      item.result.metadata["truncated"] = true;
    }
    if (item.result.metadata.contains("artifacts")) {
      auto artifact = base_event("artifact/snapshot", session, trace, run, turn);
      artifact.step_id = step;
      artifact.tool_call_id = item.call.id;
      artifact.data = {{"tool_call_id", item.call.id.str()},
                       {"name", item.call.name},
                       {"artifacts", item.result.metadata["artifacts"]}};
      append_event(std::move(artifact));
    }
    auto event = base_event("tool/result", session, trace, run, turn);
    event.step_id = step;
    event.tool_call_id = item.call.id;
    event.data = {{"tool_call_id", item.call.id.str()},
                  {"name", item.call.name},
                  {"success", item.result.success},
                  {"status", item.result.status},
                  {"content", item.result.content},
                  {"metadata", item.result.metadata}};
    if (item.plan) {
      event.data["canonical_plan_hash"] = item.plan->hash;
      event.data["idempotency_key"] = item.plan->idempotency_key;
    }
    append_event(std::move(event));
    results.push_back(item.result);
  }
  return results;
}

} // namespace snow
