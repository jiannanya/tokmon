#include <snow/protocol.hpp>
#include <snow/assembly.hpp>

#include <tokmon/common/types.hpp>

#include <string>
#include <algorithm>
#include <future>
#include <thread>

namespace snow {
namespace {

std::string base64(std::span<const std::byte> bytes) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((bytes.size() + 2) / 3) * 4);
  for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
    const auto a = std::to_integer<unsigned char>(bytes[offset]);
    const auto b = offset + 1 < bytes.size()
                       ? std::to_integer<unsigned char>(bytes[offset + 1])
                       : 0;
    const auto c = offset + 2 < bytes.size()
                       ? std::to_integer<unsigned char>(bytes[offset + 2])
                       : 0;
    result.push_back(alphabet[a >> 2]);
    result.push_back(alphabet[((a & 0x03U) << 4U) | (b >> 4U)]);
    result.push_back(offset + 1 < bytes.size()
                         ? alphabet[((b & 0x0fU) << 2U) | (c >> 6U)]
                         : '=');
    result.push_back(offset + 2 < bytes.size() ? alphabet[c & 0x3fU] : '=');
  }
  return result;
}

} // namespace

bool ProtocolApprovalService::approve(
    const ToolDefinition& tool, const tokmon::Json& canonical_arguments,
    std::string_view reason, const Details& details) {
  const auto id = tokmon::make_uuid();
  NotificationSink sink;
  {
    std::lock_guard lock(mutex_);
    pending_.emplace(id, Pending{});
    sink = sink_;
  }
  if (!sink) {
    std::lock_guard lock(mutex_);
    pending_.erase(id);
    return false;
  }
  sink({{"jsonrpc", "2.0"},
        {"method", "approval.request"},
        {"params", {{"approval_id", id},
                    {"tool", tool.name},
                    {"arguments", canonical_arguments},
                    {"reason", reason},
                    {"canonical_plan_hash", details.canonical_plan_hash},
                    {"idempotency_key", details.idempotency_key},
                    {"sandbox_plan", details.sandbox_plan}}}});
  std::unique_lock lock(mutex_);
  const auto decided = condition_.wait_for(lock, timeout_, [&] {
    const auto found = pending_.find(id);
    return found == pending_.end() || found->second.decision.has_value();
  });
  const auto found = pending_.find(id);
  const bool result = decided && found != pending_.end() &&
                      found->second.decision.value_or(false);
  if (found != pending_.end()) pending_.erase(found);
  return result;
}

void ProtocolApprovalService::set_notification_sink(NotificationSink sink) {
  std::lock_guard lock(mutex_);
  sink_ = std::move(sink);
}

bool ProtocolApprovalService::resolve(std::string_view approval_id,
                                      bool approved) {
  {
    std::lock_guard lock(mutex_);
    const auto found = pending_.find(approval_id);
    if (found == pending_.end() || found->second.decision) return false;
    found->second.decision = approved;
  }
  condition_.notify_all();
  return true;
}

void ProtocolApprovalService::cancel_all() {
  {
    std::lock_guard lock(mutex_);
    for (auto& [_, pending] : pending_) pending.decision = false;
  }
  condition_.notify_all();
}

void ProtocolServer::serve(std::istream& input, std::ostream& output) {
  std::mutex output_mutex;
  const auto send = [&](const tokmon::Json& message) {
    std::lock_guard lock(output_mutex);
    output << message.dump() << '\n' << std::flush;
  };
  if (approvals_) approvals_->set_notification_sink(send);

  std::vector<std::future<void>> workers;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    if (line.size() > 8U * 1024U * 1024U) {
      send({{"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", {{"code", "protocol.frame-limit"},
                       {"message", "NDJSON frame exceeds 8 MiB"}}}});
      continue;
    }
    std::erase_if(workers, [](auto& worker) {
      return worker.wait_for(std::chrono::milliseconds(0)) ==
             std::future_status::ready;
    });
    if (workers.size() >= 64) {
      send({{"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", {{"code", "protocol.backpressure"},
                       {"message", "too many in-flight requests"}}}});
      continue;
    }
    workers.push_back(std::async(
        std::launch::async, [this, line, send] {
          tokmon::Json response;
          tokmon::Json response_id = nullptr;
          try {
            const auto request = tokmon::Json::parse(line);
            response_id = request.value("id", tokmon::Json(nullptr));
            response = handle(request);
          } catch (const tokmon::Error& error) {
            response = {{"jsonrpc", "2.0"},
                        {"id", response_id},
                        {"error", {{"code", error.code()},
                                   {"message", error.what()},
                                   {"data", error.details()}}}};
          } catch (const std::exception& error) {
            response = {{"jsonrpc", "2.0"},
                        {"id", response_id},
                        {"error", {{"code", "internal"},
                                   {"message", error.what()}}}};
          }
          if (!response.is_null()) send(response);
        }));
  }
  if (approvals_) approvals_->cancel_all();
  workers.clear(); // future destruction joins every in-flight request.
}

tokmon::Json ProtocolServer::handle(const tokmon::Json& request) {
  if (request.value("jsonrpc", "") != "2.0") {
    throw tokmon::Error("protocol.version", "jsonrpc must be 2.0");
  }
  const auto id = request.value("id", tokmon::Json(nullptr));
  const auto method = request.at("method").get<std::string>();
  const auto params = request.value("params", tokmon::Json::object());
  tokmon::Json result;
  auto& agent = assembly_ ? assembly_->agent() : *agent_;

  if (method == "initialize") {
    const auto client_min = params.value("protocol_min", 1);
    const auto client_max = params.value("protocol_max", 1);
    if (client_min > 1 || client_max < 1) {
      throw tokmon::Error("protocol.incompatible",
                          "client and server protocol ranges do not overlap");
    }
    std::vector<std::string> capabilities{
        "session.create", "session.list", "session.resume", "session.fork",
        "session.close", "session.events", "session.transcript",
        "session.replay", "turn.start", "turn.cancel", "turn.steer",
        "artifact.read", "diagnostics.inspect"};
    if (approvals_) capabilities.push_back("approval.respond");
    if (assembly_) {
      capabilities.insert(capabilities.end(),
                          {"composition.inspect", "composition.stage",
                           "composition.apply"});
    }
    result = {{"protocol", {{"min", 1}, {"max", 1}}},
              {"selected_protocol", 1},
              {"server", {{"name", "snow"}, {"version", "1.0.0"}}},
              {"capabilities", std::move(capabilities)}};
  } else if (method == "session.create") {
    result = {
        {"session_id",
         agent.create_session(params.value("metadata", tokmon::Json::object()))
             .str()}};
  } else if (method == "session.list") {
    result = tokmon::Json::array();
    for (const auto& session :
         agent.sessions(params.value("limit", std::size_t{100}))) {
      tokmon::Json item{{"session_id", session.id.str()},
                        {"created_at", session.created_at},
                        {"header", session.header},
                        {"last_seq", session.last_seq},
                        {"closed", session.closed_at.has_value()}};
      if (session.parent_id)
        item["parent_session_id"] = session.parent_id->str();
      if (session.closed_at) item["closed_at"] = *session.closed_at;
      result.push_back(std::move(item));
    }
  } else if (method == "session.resume") {
    const tokmon::SessionId session(params.at("session_id").get<std::string>());
    const auto events = agent.events(session, params.value("after", 0ULL));
    result = {{"session_id", session.str()},
              {"events", events},
              {"last_seq", events.empty()
                               ? params.value("after", 0ULL)
                               : events.back().seq}};
  } else if (method == "session.fork") {
    const tokmon::SessionId parent(params.at("session_id").get<std::string>());
    const auto child = agent.fork_session(
        parent, params.value("metadata", tokmon::Json::object()),
        params.value("seed_seq", 0ULL));
    result = {{"session_id", child.str()}, {"parent_session_id", parent.str()}};
  } else if (method == "session.close") {
    const tokmon::SessionId session(params.at("session_id").get<std::string>());
    agent.close_session(session);
    result = {{"closed", true}, {"session_id", session.str()}};
  } else if (method == "turn.start") {
    const tokmon::SessionId session(params.at("session_id").get<std::string>());
    std::stop_source source;
    {
      std::lock_guard lock(turns_mutex_);
      if (active_turns_.contains(session.str())) {
        throw tokmon::Error("snow.turn.active",
                            "the session already has an active turn");
      }
      active_turns_[session.str()] = source;
    }
    struct ActiveTurn {
      ProtocolServer* server;
      std::string session;
      ~ActiveTurn() {
        std::lock_guard lock(server->turns_mutex_);
        server->active_turns_.erase(session);
      }
    } active{this, session.str()};
    RunOptions options;
    options.model = params.value("model", "");
    options.max_steps = params.value("max_steps", 32U);
    options.model_parameters =
        params.value("model_parameters", tokmon::Json::object());
    options.attachments =
        params.value("attachments", tokmon::Json::array());
    if (!options.attachments.is_array() || options.attachments.size() > 8)
      throw tokmon::Error("snow.turn.attachments",
                          "attachments must be an array with at most 8 items");
    std::size_t attachment_bytes = 0;
    for (const auto& attachment : options.attachments) {
      if (!attachment.is_object() ||
          !attachment.contains("content") ||
          !attachment.at("content").is_string())
        throw tokmon::Error("snow.turn.attachments",
                            "each attachment must contain text content");
      attachment_bytes += attachment.at("content").get_ref<
                              const std::string&>()
                              .size();
    }
    if (attachment_bytes > 2U * 1024U * 1024U)
      throw tokmon::Error("snow.turn.attachments",
                          "attachment content exceeds 2 MiB");
    const auto run = agent.run(
        session, params.at("message").get<std::string>(), options,
        source.get_token());
    result = {{"run_id", run.run_id.str()},
              {"turn_id", run.turn_id.str()},
              {"reason", to_string(run.reason)},
              {"final_text", run.final_text},
              {"last_seq", run.last_seq}};
  } else if (method == "turn.cancel") {
    const auto session = params.at("session_id").get<std::string>();
    std::lock_guard lock(turns_mutex_);
    const auto found = active_turns_.find(session);
    const bool requested = found != active_turns_.end() &&
                           found->second.request_stop();
    result = {{"accepted", requested}, {"session_id", session}};
  } else if (method == "turn.steer") {
    const tokmon::SessionId session(params.at("session_id").get<std::string>());
    const auto seq =
        agent.steer(session, params.at("message").get<std::string>());
    result = {{"accepted", true},
              {"session_id", session.str()},
              {"seq", seq}};
  } else if (method == "approval.respond") {
    if (!approvals_) {
      throw tokmon::Error("snow.approval.unavailable",
                          "protocol approval service is not configured");
    }
    const auto approval_id = params.at("approval_id").get<std::string>();
    const auto resolved = approvals_->resolve(
        approval_id, params.value("approved", false));
    result = {{"resolved", resolved}, {"approval_id", approval_id}};
  } else if (method == "session.events") {
    const tokmon::SessionId session(params.at("session_id").get<std::string>());
    auto events = agent.events(session, params.value("after", 0ULL));
    const auto limit = std::clamp(params.value("limit", 10000U), 1U, 10000U);
    if (events.size() > limit) events.resize(limit);
    result = std::move(events);
  } else if (method == "session.transcript") {
    const tokmon::SessionId session(params.at("session_id").get<std::string>());
    result = agent.transcript(session);
  } else if (method == "session.replay") {
    const tokmon::SessionId session(params.at("session_id").get<std::string>());
    const auto requested = params.value("level", "R2");
    const auto level = requested == "R0"
                           ? ReplayLevel::transcript
                           : (requested == "R1"
                                  ? ReplayLevel::request_reconstruction
                                  : ReplayLevel::control);
    const auto replay = agent.replay(session, level);
    result = {{"level", to_string(replay.level)},
              {"deterministic", replay.deterministic},
              {"degradations", replay.degradations},
              {"transcript", replay.transcript},
              {"requests", replay.requests},
              {"control", replay.control},
              {"final_state", replay.final_state}};
  } else if (method == "artifact.read") {
    if (!artifacts_)
      throw tokmon::Error("snow.artifact.unavailable",
                          "artifact service is not configured");
    BlobReference reference;
    reference.sha256 = params.at("sha256").get<std::string>();
    reference.id = "sha256:" + reference.sha256;
    reference.bytes = params.value("bytes", 0ULL);
    reference.media_type =
        params.value("media_type", "application/octet-stream");
    const auto content = artifacts_->read(reference);
    const auto offset = params.value("offset", 0ULL);
    const auto requested = std::clamp(
        params.value("limit", 256U * 1024U), 1U, 1024U * 1024U);
    if (offset > content.size())
      throw tokmon::Error("snow.artifact.offset",
                          "artifact offset exceeds blob size");
    const auto count = std::min<std::size_t>(
        requested, content.size() - static_cast<std::size_t>(offset));
    result = {{"sha256", reference.sha256},
              {"offset", offset},
              {"next_offset", offset + count},
              {"eof", offset + count == content.size()},
              {"bytes", content.size()},
              {"media_type", reference.media_type},
              {"data_base64",
               base64(std::span(content).subspan(
                   static_cast<std::size_t>(offset), count))}};
  } else if (method == "diagnostics.inspect") {
    result = runtime_ ? runtime_->inspect()
                      : tokmon::Json{{"status", "runtime-unavailable"}};
  } else if (method == "composition.inspect") {
    if (!assembly_)
      throw tokmon::Error("snow.composition.unavailable",
                          "composition controller is not configured");
    const auto& report = assembly_->composition_report();
    result = {{"composition_id", report.composition_id},
              {"epoch", assembly_->arche_runtime().epoch()},
              {"runtime", assembly_->arche_runtime().inspect()}};
  } else if (method == "composition.stage") {
    if (!assembly_)
      throw tokmon::Error("snow.composition.unavailable",
                          "composition controller is not configured");
    const tokmon::SessionId session(params.at("session_id").get<std::string>());
    const auto proposal =
        arche::EvolutionProposal::parse(params.at("proposal"));
    result = assembly_->stage_package(
        session, params.at("package_root").get<std::string>(), proposal,
        params.value("permission_increase_approved", false));
  } else if (method == "composition.apply") {
    if (!assembly_)
      throw tokmon::Error("snow.composition.unavailable",
                          "composition controller is not configured");
    if (!params.value("approved", false))
      throw tokmon::Error("snow.composition.approval",
                          "composition apply requires explicit approval");
    const tokmon::SessionId session(params.at("session_id").get<std::string>());
    const auto desired =
        arche::DesiredComposition::parse(params.at("composition"));
    const auto report = assembly_->apply_composition(session, desired);
    tokmon::Json actions = tokmon::Json::array();
    for (const auto& action : report.actions)
      actions.push_back({{"action", action.action},
                         {"instance", action.instance},
                         {"from", action.from}, {"to", action.to}});
    result = {{"composition_id", report.composition_id},
              {"epoch_before", report.epoch_before},
              {"epoch_after", report.epoch_after},
              {"actions", std::move(actions)}};
  } else {
    throw tokmon::Error("protocol.method", "unknown method: " + method);
  }
  return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

} // namespace snow
