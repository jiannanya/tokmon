#include <snow/replay.hpp>

#include <tokmon/common/types.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace snow {
namespace {

void issue(ValidationReport& report, const TrajectoryEvent& event,
           std::string code, std::string message, bool fatal = true) {
  report.issues.push_back(
      {event.seq, std::move(code), std::move(message), fatal});
}

std::string optional_value(const auto& value) {
  return value ? value->str() : std::string{};
}

} // namespace

bool ValidationReport::valid() const noexcept {
  return std::ranges::none_of(issues, &ValidationIssue::fatal);
}

void ValidationReport::throw_if_invalid() const {
  const auto found = std::ranges::find_if(issues, &ValidationIssue::fatal);
  if (found != issues.end()) {
    throw tokmon::Error("snow.replay.invalid",
                        found->code + " at seq " +
                            std::to_string(found->seq) + ": " +
                            found->message);
  }
}

TrajectoryValidator::TrajectoryValidator() {
  for (const auto* family :
       {"session/", "run/", "turn/", "step/", "inbox/", "user/",
        "request/", "model/", "assistant/", "tool/", "approval/",
        "sandbox/", "artifact/", "context/", "memory/", "skill/",
        "child/", "composition/", "recovery/", "agent/", "telemetry/"}) {
    families_.insert(family);
  }
}

void TrajectoryValidator::register_family(std::string prefix) {
  if (prefix.empty() || !prefix.ends_with('/')) {
    throw tokmon::Error("snow.event.family",
                        "event family must be a non-empty prefix ending in /");
  }
  families_.insert(std::move(prefix));
}

bool TrajectoryValidator::known(std::string_view type) const {
  return std::ranges::any_of(families_, [&](const auto& prefix) {
    return type.starts_with(prefix);
  });
}

ValidationReport TrajectoryValidator::validate(
    const std::vector<TrajectoryEvent>& events) const {
  ValidationReport report;
  if (events.empty()) return report;

  std::optional<tokmon::SessionId> session;
  std::uint64_t previous = 0;
  std::set<std::string> open_runs;
  std::set<std::string> open_turns;
  std::set<std::string> open_steps;
  std::map<std::string, std::uint64_t, std::less<>> tool_calls;
  std::set<std::string> tool_results;

  for (const auto& event : events) {
    if (!session) session = event.session_id;
    if (event.session_id != *session) {
      issue(report, event, "session-mismatch",
            "one replay stream must contain exactly one session");
    }
    if (event.seq == 0 || (previous != 0 && event.seq != previous + 1)) {
      issue(report, event, "seq-gap",
            "event sequence is not strictly contiguous");
    }
    previous = event.seq;
    if (event.type.empty() || event.trace_id.empty() ||
        event.producer_fiber.empty()) {
      issue(report, event, "envelope",
            "type, trace_id and producer_fiber are required");
    }
    if (!known(event.type) && !event.ignorable) {
      issue(report, event, "unknown-required-event",
            "required event family is not registered: " + event.type);
    }
    for (const auto source : event.source_event_seqs) {
      if (source == 0 || source >= event.seq) {
        issue(report, event, "invalid-provenance",
              "source_event_seqs must refer to earlier durable events");
      }
    }

    const auto run = optional_value(event.run_id);
    const auto turn = optional_value(event.turn_id);
    const auto step = optional_value(event.step_id);
    if (event.type == "run/start") {
      if (run.empty() || !open_runs.insert(run).second)
        issue(report, event, "run-nesting", "run/start is duplicated");
    } else if (event.type == "run/end") {
      if (run.empty() || open_runs.erase(run) != 1)
        issue(report, event, "run-nesting", "run/end has no matching start");
    } else if (event.type == "turn/start") {
      if (turn.empty() || !open_turns.insert(turn).second)
        issue(report, event, "turn-nesting", "turn/start is duplicated");
    } else if (event.type == "turn/end") {
      if (turn.empty() || open_turns.erase(turn) != 1)
        issue(report, event, "turn-nesting", "turn/end has no matching start");
    } else if (event.type == "step/start") {
      if (step.empty() || !open_steps.insert(step).second)
        issue(report, event, "step-nesting", "step/start is duplicated");
    } else if (event.type == "step/end") {
      if (step.empty() || open_steps.erase(step) != 1)
        issue(report, event, "step-nesting", "step/end has no matching start");
    } else if (event.type == "tool/call") {
      const auto id = optional_value(event.tool_call_id);
      if (id.empty() || !tool_calls.emplace(id, event.seq).second)
        issue(report, event, "tool-pair", "tool call id is missing or duplicated");
    } else if (event.type == "tool/result") {
      const auto id = optional_value(event.tool_call_id);
      if (!tool_calls.contains(id) || !tool_results.insert(id).second)
        issue(report, event, "tool-pair",
              "tool result has no call or is duplicated");
    }
  }

  for (const auto& [id, call_seq] : tool_calls) {
    if (!tool_results.contains(id)) {
      report.issues.push_back(
          {call_seq, "tool-pair", "tool call has no durable result", true});
    }
  }
  if (!open_steps.empty())
    report.issues.push_back(
        {previous, "step-nesting", "replay ends with an open step", true});
  if (!open_turns.empty())
    report.issues.push_back(
        {previous, "turn-nesting", "replay ends with an open turn", true});
  if (!open_runs.empty())
    report.issues.push_back(
        {previous, "run-nesting", "replay ends with an open run", true});
  return report;
}

ReplayEngine::ReplayEngine(TrajectoryValidator validator)
    : validator_(std::move(validator)) {}

ReplayReport ReplayEngine::replay(
    const std::vector<TrajectoryEvent>& events, ReplayLevel level) const {
  validator_.validate(events).throw_if_invalid();
  ReplayReport report;
  report.level = level;
  for (const auto& item : surface_.project(events)) report.transcript.push_back(item);
  if (level == ReplayLevel::transcript) return report;

  std::map<std::string, tokmon::Json, std::less<>> headers;
  for (const auto& event : events) {
    if (event.type == "request/header" && event.model_call_id) {
      headers[event.model_call_id->str()] = event.data;
      for (const auto* lock : {"composition_epoch", "system_prompt_hash",
                               "tools", "context_sources"}) {
        if (!event.data.contains(lock)) {
          report.deterministic = false;
          report.degradations.push_back(
              "request/header is missing replay lock '" +
              std::string(lock) + "' at seq " + std::to_string(event.seq));
        }
      }
    } else if (event.type == "request/context" && event.model_call_id) {
      const auto id = event.model_call_id->str();
      report.requests.push_back(
          {{"model_call_id", id},
           {"header", headers.contains(id) ? headers[id] : tokmon::Json(nullptr)},
           {"context", event.data},
           {"source_seq", event.seq}});
    }
  }
  if (level == ReplayLevel::request_reconstruction) return report;

  std::map<std::string, tokmon::Json, std::less<>> turns;
  for (const auto& event : events) {
    if (event.type == "assistant/message") {
      report.control.push_back(
          {{"op", "model-result"},
           {"seq", event.seq},
           {"step_id", optional_value(event.step_id)},
           {"data", event.data}});
    } else if (event.type == "tool/policy-decision" ||
               event.type == "approval/result" ||
               event.type == "sandbox/plan" ||
               event.type == "tool/result") {
      report.control.push_back(
          {{"op", event.type}, {"seq", event.seq}, {"data", event.data}});
    } else if (event.type == "turn/end" && event.turn_id) {
      turns[event.turn_id->str()] = event.data;
    }
  }
  report.final_state = {{"turns", turns},
                        {"last_seq", events.empty() ? 0 : events.back().seq},
                        {"valid", true}};
  return report;
}

std::string_view to_string(ReplayLevel level) noexcept {
  switch (level) {
  case ReplayLevel::transcript: return "R0";
  case ReplayLevel::request_reconstruction: return "R1";
  case ReplayLevel::control: return "R2";
  }
  return "R0";
}

} // namespace snow
