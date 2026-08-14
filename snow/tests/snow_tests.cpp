#include <snow/assembly.hpp>
#include <snow/artifact.hpp>
#include <snow/config.hpp>
#include <snow/c_api.h>
#include <snow/protocol.hpp>
#include <snow/replay.hpp>
#include <tokmon/common/digest.hpp>
#include <tokmon/common/files.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>

namespace {

class ParallelProbeTool final : public snow::Tool {
public:
  ParallelProbeTool(std::atomic_int& active, std::atomic_int& maximum)
      : active_(active), maximum_(maximum) {
    definition_ = {"parallel_probe", "test deterministic parallel execution",
                   {{"type", "object"},
                    {"properties", {{"delay_ms", {{"type", "integer"}}}}},
                    {"required", {"delay_ms"}},
                    {"additionalProperties", false}},
                   true, true};
  }
  const snow::ToolDefinition& definition() const override { return definition_; }
  snow::ToolResult execute(const tokmon::Json& arguments,
                           std::stop_token stop) override {
    const auto current = active_.fetch_add(1) + 1;
    auto observed = maximum_.load();
    while (current > observed &&
           !maximum_.compare_exchange_weak(observed, current)) {
    }
    const auto delay = std::chrono::milliseconds(
        arguments.at("delay_ms").get<int>());
    const auto end = std::chrono::steady_clock::now() + delay;
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < end)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    active_.fetch_sub(1);
    return {!stop.stop_requested(), stop.stop_requested() ? "aborted" : "ok",
            std::to_string(delay.count())};
  }

private:
  snow::ToolDefinition definition_;
  std::atomic_int& active_;
  std::atomic_int& maximum_;
};

class CancellableModel final : public snow::ModelProvider {
public:
  explicit CancellableModel(std::atomic_bool& entered) : entered_(entered) {}
  std::string id() const override { return "cancellable-test"; }
  snow::ModelResponse complete(const snow::ModelRequest&,
                               snow::ModelChunkCallback,
                               std::stop_token stop = {}) override {
    entered_ = true;
    while (!stop.stop_requested())
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    throw tokmon::Error("snow.cancelled", "test request cancelled");
  }

private:
  std::atomic_bool& entered_;
};

} // namespace

int main() {
  assert(tokmon::sha256_hex("abc") ==
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const auto temporary =
      std::filesystem::temp_directory_path() / ("tokmon-test-" +
                                                tokmon::make_uuid());
  std::filesystem::create_directories(temporary);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{temporary};

  {
    const auto standalone = snow::ConfigLayout::resolve(temporary);
    assert(standalone.config_root.filename() == ".snow");
    const auto integrated = snow::ConfigLayout::resolve(temporary, ".tokmon");
    assert(integrated.config_root.filename() == ".tokmon");
    assert(integrated.mcp.filename() == "mcp.json");
    bool rejected = false;
    try {
      (void)snow::ConfigLayout::resolve(temporary, "../escape");
    } catch (const tokmon::Error&) {
      rejected = true;
    }
    assert(rejected);
    const auto commented = integrated.config_root / "commented.json";
    tokmon::write_text_file_atomic(commented, "{\n// forbidden\n\"x\": 1\n}");
    bool comment_rejected = false;
    try {
      (void)snow::load_json_config(commented);
    } catch (const tokmon::Error& error) {
      comment_rejected = error.code() == "snow.config.json";
    }
    assert(comment_rejected);
  }

  {
    const auto workspace = temporary / "c-api-workspace";
    std::filesystem::create_directories(workspace);
    const auto bootstrap =
        tokmon::Json{{"schema", "org.tokmon.snow.bootstrap/v1"},
                     {"workspace", workspace.generic_string()},
                     {"config_dir_name", ".third-party"},
                     {"data_root",
                      (temporary / "c-api-data").generic_string()}}
            .dump();
    snow_host_v1* host = nullptr;
    assert(snow_abi_version_v1() == SNOW_C_ABI_VERSION_V1);
    assert(snow_host_create_v1(bootstrap.data(), bootstrap.size(), &host) ==
           SNOW_STATUS_OK_V1);
    assert(host);
    const auto invoke = [&](tokmon::Json request) {
      const auto encoded = request.dump();
      snow_buffer_v1 response{};
      assert(snow_host_invoke_v1(host, encoded.data(), encoded.size(),
                                 &response) == SNOW_STATUS_OK_V1);
      const auto result = tokmon::Json::parse(
          std::string_view(response.data, response.size));
      snow_buffer_release_v1(&response);
      return result;
    };
    const auto initialized = invoke(
        {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});
    assert(initialized["result"]["selected_protocol"] == 1);
    const auto created = invoke({{"jsonrpc", "2.0"},
                                 {"id", 2},
                                 {"method", "session.create"}});
    const auto c_session =
        created["result"]["session_id"].get<std::string>();
    const auto run = invoke(
        {{"jsonrpc", "2.0"},
         {"id", 3},
         {"method", "turn.start"},
         {"params", {{"session_id", c_session}, {"message", "hello"}}}});
    assert(run["result"]["reason"] == "completed");
    const auto c_events = invoke(
        {{"jsonrpc", "2.0"},
         {"id", 4},
         {"method", "session.events"},
         {"params", {{"session_id", c_session}}}});
    assert(!c_events["result"].empty());
    snow_host_destroy_v1(host);
  }

  {
    const auto workspace = temporary / "mcp-workspace";
    const auto config_root = workspace / ".snow";
    std::filesystem::create_directories(config_root);
    tokmon::write_text_file_atomic(
        config_root / "mcp.json",
        tokmon::Json{{"schema", "org.tokmon.snow.mcp/v1"},
                     {"servers",
                      {{{"id", "fixture"},
                        {"command", SNOW_MCP_FIXTURE_PATH},
                        {"args", tokmon::Json::array()},
                        {"cwd", "."},
                        {"request_timeout_ms", 5000}}}}}
            .dump(2));
    snow::ModelResponse call;
    call.finish_reason = "tool_calls";
    call.tool_calls = {{tokmon::ToolCallId("mcp-call"),
                        "mcp__fixture__echo", {{"text", "hello"}}}};
    auto mcp_model = std::make_shared<snow::ScriptedModelProvider>(
        std::vector<snow::ModelResponse>{call, {.content = "MCP complete"}});
    auto approve_mcp = std::make_shared<snow::CallbackApproval>(
        [](const snow::ToolDefinition&, const tokmon::Json&,
           std::string_view) { return true; });
    snow::BootstrapConfig mcp_config;
    mcp_config.workspace = workspace;
    mcp_config.data_root = temporary / "mcp-data";
    snow::Assembly mcp_assembly(mcp_config, mcp_model, approve_mcp);
    const auto registry = mcp_assembly.arche_runtime().root_context()->require<
        snow::ToolRegistry>("snow.tools", "^1.0");
    const auto remote = registry->find("mcp__fixture__echo");
    assert(remote);
    // The remote readOnlyHint is deliberately not trusted by local policy.
    assert(!remote->definition().read_only);
    const auto mcp_session = mcp_assembly.agent().create_session();
    const auto mcp_result = mcp_assembly.agent().run(mcp_session, "use MCP");
    assert(mcp_result.reason == snow::TurnEndReason::completed);
    const auto mcp_events = mcp_assembly.agent().events(mcp_session);
    assert(std::ranges::any_of(mcp_events, [](const auto& event) {
      return event.type == "tool/result" &&
             event.data.value("content", "") == "mcp:hello" &&
             event.data["metadata"].value("mcp_server", "") == "fixture";
    }));
  }

  snow::ModelResponse tool_response;
  tool_response.tool_calls.push_back(
      {tokmon::ToolCallId("call-1"), "write_file",
       {{"path", "result.txt"}, {"content", "written by Snow"}}});
  tool_response.finish_reason = "tool_calls";
  snow::ModelResponse final_response;
  final_response.content = "The file was written successfully.";

  auto model = std::make_shared<snow::ScriptedModelProvider>(
      std::vector<snow::ModelResponse>{tool_response, final_response});
  auto approval = std::make_shared<snow::CallbackApproval>(
      [](const snow::ToolDefinition&, const tokmon::Json&,
         std::string_view) { return true; });
  snow::BootstrapConfig config;
  config.workspace = temporary;
  config.config_dir_name = ".tokmon";
  config.data_root = temporary / "data";
  std::filesystem::create_directories(temporary / ".tokmon");
  tokmon::write_text_file_atomic(temporary / ".tokmon" / "instructions.md",
                                 "Always mention the workspace.");
  snow::Assembly assembly(config, model, approval);
  bool second_writer_rejected = false;
  try {
    snow::TrajectoryJournal competing(config.data_root / "snow.db");
  } catch (const tokmon::Error& error) {
    second_writer_rejected = error.code() == "snow.writer.locked";
  }
  assert(second_writer_rejected);

  const auto session =
      assembly.agent().create_session({{"test", "full-trajectory"}});
  const auto result =
      assembly.agent().run(session, "Create result.txt", {.model = "fake"});
  assert(result.reason == snow::TurnEndReason::completed);
  assert(result.final_text == "The file was written successfully.");
  assert(tokmon::read_text_file(temporary / "result.txt") ==
         "written by Snow");

  const auto events = assembly.agent().events(session);
  assert(!events.empty());
  for (std::size_t index = 0; index < events.size(); ++index) {
    assert(events[index].seq == index + 1);
  }
  const auto has = [&](std::string_view type) {
    return std::ranges::any_of(events, [&](const auto& event) {
      return event.type == type;
    });
  };
  assert(has("request/header"));
  assert(has("request/context"));
  assert(has("tool/policy-decision"));
  assert(has("tool/normalized"));
  assert(has("tool/admission"));
  assert(has("approval/request"));
  assert(has("approval/result"));
  assert(has("sandbox/plan"));
  assert(has("tool/dispatch"));
  assert(has("artifact/snapshot"));
  assert(has("tool/result"));
  assert(has("turn/end"));
  assert(has("run/end"));
  const auto completed_turn = std::ranges::find_if(events, [](const auto& event) {
    return event.type == "turn/end";
  });
  assert(completed_turn != events.end());
  assert(completed_turn->data.value("elapsed_ms", std::int64_t{-1}) >= 0);

  const auto transcript = assembly.agent().transcript(session);
  assert(transcript.size() >= 4);
  assert(model->requests().size() == 2);
  assert(model->requests()[0].messages[0]["role"] == "system");
  assert(model->requests()[0].messages[0]["content"]
             .get<std::string>()
             .find("Always mention the workspace.") != std::string::npos);
  assert(model->requests()[1].messages.back()["role"] == "tool");

  const auto request_header = std::ranges::find_if(events, [](const auto& event) {
    return event.type == "request/header";
  });
  assert(request_header != events.end());
  assert(request_header->data["system_prompt_hash"]
             .get<std::string>()
             .starts_with("sha256:"));
  const auto normalized = std::ranges::find_if(events, [](const auto& event) {
    return event.type == "tool/normalized";
  });
  const auto tool_result = std::ranges::find_if(events, [](const auto& event) {
    return event.type == "tool/result";
  });
  assert(normalized != events.end() && tool_result != events.end());
  assert(normalized->data["canonical_plan_hash"] ==
         tool_result->data["canonical_plan_hash"]);
  assert(!tool_result->data["metadata"]["artifacts"].contains("preimage"));
  assert(tool_result->data["metadata"]["artifacts"].contains("postimage"));

  const auto replay = assembly.agent().replay(session, snow::ReplayLevel::control);
  assert(replay.deterministic);
  assert(replay.requests.size() == 2);
  assert(!replay.control.empty());

  auto protocol_artifacts = assembly.arche_runtime().root_context()
                                ->require<snow::ArtifactStore>("snow.artifacts",
                                                               "^1.0");
  snow::ProtocolServer server(assembly.agent(), {}, protocol_artifacts.shared(),
                              &assembly.arche_runtime(), &assembly);
  const auto response =
      server.handle({{"jsonrpc", "2.0"},
                     {"id", 1},
                     {"method", "session.events"},
                     {"params",
                      {{"session_id", session.str()}, {"after", 0}}}});
  assert(response["result"].size() == events.size());

  const auto fork_response = server.handle(
      {{"jsonrpc", "2.0"},
       {"id", 2},
       {"method", "session.fork"},
       {"params", {{"session_id", session.str()}, {"seed_seq", result.last_seq}}}});
  const tokmon::SessionId child(
      fork_response["result"]["session_id"].get<std::string>());
  assert(assembly.agent().events(child).size() == 2);
  const auto replay_response = server.handle(
      {{"jsonrpc", "2.0"},
       {"id", 3},
       {"method", "session.replay"},
       {"params", {{"session_id", session.str()}, {"level", "R1"}}}});
  assert(replay_response["result"]["requests"].size() == 2);
  (void)server.handle({{"jsonrpc", "2.0"},
                       {"id", 4},
                       {"method", "session.close"},
                       {"params", {{"session_id", child.str()}}}});
  assert(assembly.agent().events(child).back().type == "session/closed");
  const auto listed_sessions = server.handle(
      {{"jsonrpc", "2.0"}, {"id", 41}, {"method", "session.list"},
       {"params", {{"limit", 100}}}});
  assert(listed_sessions["result"].is_array());
  assert(std::ranges::any_of(
      listed_sessions["result"], [&](const auto& listed) {
        return listed.value("session_id", "") == child.str() &&
               listed.value("closed", false);
      }));
  const auto protocol_blob = protocol_artifacts->put_text("chunked artifact");
  const auto blob_response = server.handle(
      {{"jsonrpc", "2.0"},
       {"id", 5},
       {"method", "artifact.read"},
       {"params", {{"sha256", protocol_blob.sha256},
                    {"media_type", protocol_blob.media_type},
                    {"offset", 0}, {"limit", 7}}}});
  assert(blob_response["result"]["data_base64"] == "Y2h1bmtlZA==");
  assert(!blob_response["result"]["eof"]);
  const auto diagnostics = server.handle(
      {{"jsonrpc", "2.0"}, {"id", 6},
       {"method", "diagnostics.inspect"}});
  assert(diagnostics["result"].contains("fibers"));
  const auto composition_inspect = server.handle(
      {{"jsonrpc", "2.0"}, {"id", 7},
       {"method", "composition.inspect"}});
  assert(composition_inspect["result"]["composition_id"] ==
         "org.tokmon.snow.default");
  const tokmon::Json unchanged_composition = {
      {"schema", "org.tokmon.arche.composition/v1"},
      {"id", "snow.test.reconcile"},
      {"plugins",
       {{{"instance", "session"},
         {"package", "org.tokmon.snow.session.sqlite@1.0.0"},
         {"realm", "storage"}},
        {{"instance", "storage"},
         {"package", "org.tokmon.snow.storage.default@1.0.0"},
         {"realm", "storage"}},
        {{"instance", "model"},
         {"package", "org.tokmon.snow.model.configured@1.0.0"},
         {"realm", "model"}},
        {{"instance", "tools"},
         {"package", "org.tokmon.snow.tools.default@1.0.0"},
         {"realm", "tools"}},
        {{"instance", "policy"},
         {"package", "org.tokmon.snow.policy.default@1.0.0"},
         {"realm", "policy"}},
        {{"instance", "agent"},
         {"package", "org.tokmon.snow.loop.direct@1.0.0"},
         {"realm", "agent"}}}}};
  const auto composition_apply = server.handle(
      {{"jsonrpc", "2.0"}, {"id", 8},
       {"method", "composition.apply"},
       {"params", {{"session_id", session.str()},
                    {"approved", true},
                    {"composition", unchanged_composition}}}});
  assert(composition_apply["result"]["actions"].empty());
  assert(std::filesystem::exists(
      config.data_root / "plugins" / "composition.lock.json"));

  {
    snow::BootstrapConfig attachment_config = config;
    attachment_config.data_root = temporary / "attachment-data";
    auto attachment_model = std::make_shared<snow::ScriptedModelProvider>(
        std::vector<snow::ModelResponse>{{.content = "attachment received"}});
    snow::Assembly attachment_assembly(attachment_config, attachment_model);
    snow::ProtocolServer attachment_server(attachment_assembly.agent());
    const auto attachment_session =
        attachment_assembly.agent().create_session({{"title", "attachments"}});
    const tokmon::Json attachment =
        {{"name", "context.txt"},
         {"sha256", tokmon::sha256_hex("durable context")},
         {"content", "durable context"},
         {"bytes", 15}};
    const auto attachment_run = attachment_server.handle(
        {{"jsonrpc", "2.0"},
         {"id", 9},
         {"method", "turn.start"},
         {"params",
          {{"session_id", attachment_session.str()},
           {"message", "Use the attachment"},
           {"attachments", tokmon::Json::array({attachment})}}}});
    assert(attachment_run["result"]["reason"] == "completed");
    const auto attachment_events =
        attachment_assembly.agent().events(attachment_session);
    const auto user_event = std::ranges::find_if(
        attachment_events, [](const auto& event) {
          return event.type == "user/message";
        });
    assert(user_event != attachment_events.end());
    assert(user_event->data["attachments"][0]["name"] == "context.txt");
    assert(attachment_model->requests().size() == 1);
    const auto model_content = attachment_model->requests()[0]
                                   .messages.back()["content"]
                                   .get<std::string>();
    assert(model_content.find("<attachment name=\"context.txt\"") !=
           std::string::npos);
    assert(model_content.find("durable context") != std::string::npos);
    const auto summaries = attachment_assembly.agent().sessions();
    assert(std::ranges::any_of(summaries, [&](const auto& summary) {
      return summary.id == attachment_session && summary.last_seq > 0 &&
             summary.header.value("title", "") == "attachments";
    }));
  }

  {
    auto protocol_approval =
        std::make_shared<snow::ProtocolApprovalService>(
            std::chrono::seconds(2));
    std::promise<std::string> notified;
    auto notified_future = notified.get_future();
    protocol_approval->set_notification_sink(
        [&](tokmon::Json message) {
          assert(message.at("method") == "approval.request");
          notified.set_value(
              message.at("params").at("approval_id").get<std::string>());
        });
    snow::ToolDefinition approval_tool{
        "write_file", "write", tokmon::Json::object()};
    auto decision = std::async(std::launch::async, [&] {
      return protocol_approval->approve(
          approval_tool, {{"path", "approval.txt"}}, "mutable");
    });
    const auto approval_id = notified_future.get();
    assert(protocol_approval->resolve(approval_id, true));
    assert(decision.get());
    assert(!protocol_approval->resolve(approval_id, false));
  }

  {
    std::atomic_bool entered{false};
    auto cancellable = std::make_shared<CancellableModel>(entered);
    snow::BootstrapConfig cancel_config = config;
    cancel_config.data_root = temporary / "cancel-data";
    snow::Assembly cancel_assembly(cancel_config, cancellable);
    snow::ProtocolServer cancel_server(cancel_assembly.agent());
    const auto cancel_session = cancel_assembly.agent().create_session();
    auto running = std::async(std::launch::async, [&] {
      return cancel_server.handle(
          {{"jsonrpc", "2.0"},
           {"id", 10},
           {"method", "turn.start"},
           {"params", {{"session_id", cancel_session.str()},
                        {"message", "wait"}}}});
    });
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (!entered && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    assert(entered);
    const auto steered = cancel_server.handle(
        {{"jsonrpc", "2.0"},
         {"id", 12},
         {"method", "turn.steer"},
         {"params", {{"session_id", cancel_session.str()},
                      {"message", "additional direction"}}}});
    assert(steered.at("result").at("accepted"));
    const auto cancelled = cancel_server.handle(
        {{"jsonrpc", "2.0"},
         {"id", 11},
         {"method", "turn.cancel"},
         {"params", {{"session_id", cancel_session.str()}}}});
    assert(cancelled.at("result").at("accepted"));
    const auto finished = running.get();
    assert(finished.at("result").at("reason") == "aborted");
    assert(std::ranges::any_of(
        cancel_assembly.agent().events(cancel_session), [](const auto& event) {
          return event.type == "user/steer" &&
                 event.data.value("content", "") == "additional direction";
        }));
  }

#ifdef _WIN32
  {
    snow::RawTraceVault vault(temporary / "encrypted-vault", true,
                              std::chrono::hours(1));
    const auto reference = vault.put_text("test", "secret trace");
    const auto clear = vault.read(reference);
    assert(std::string(reinterpret_cast<const char*>(clear.data()), clear.size()) ==
           "secret trace");
    const auto encrypted = tokmon::read_text_file(
        temporary / "encrypted-vault" / (reference.id + ".bin"));
    assert(encrypted.find("secret trace") == std::string::npos);
  }
#endif

  {
    const auto repair_path = temporary / "repair.db";
    snow::TrajectoryJournal journal(repair_path);
    const auto repair_session = journal.create_session({{"test", "repair"}});
    const tokmon::TraceId trace(tokmon::make_uuid());
    const tokmon::RunId repair_run(tokmon::make_uuid());
    const tokmon::TurnId repair_turn(tokmon::make_uuid());
    const tokmon::StepId repair_step(tokmon::make_uuid());
    const tokmon::ToolCallId repair_call("repair-call");
    auto append = [&](std::string type) {
      snow::TrajectoryEvent event;
      event.type = std::move(type);
      event.session_id = repair_session;
      event.trace_id = trace;
      event.run_id = repair_run;
      event.turn_id = repair_turn;
      event.producer_fiber = arche::FiberId("test.repair");
      return event;
    };
    journal.append(append("run/start"));
    journal.append(append("turn/start"));
    auto step_start = append("step/start");
    step_start.step_id = repair_step;
    journal.append(std::move(step_start));
    auto call = append("tool/call");
    call.step_id = repair_step;
    call.tool_call_id = repair_call;
    call.data = {{"tool_call_id", repair_call.str()}, {"name", "write_file"}};
    journal.append(std::move(call));
    auto dispatch = append("tool/dispatch");
    dispatch.step_id = repair_step;
    dispatch.tool_call_id = repair_call;
    dispatch.data = {{"tool_call_id", repair_call.str()},
                     {"name", "write_file"},
                     {"canonical_plan_hash", "sha256:test"},
                     {"idempotency_key", "repair-key"}};
    journal.append(std::move(dispatch));
    journal.repair_interrupted_sessions();
    const auto repaired = journal.events(repair_session);
    assert(std::ranges::any_of(repaired, [](const auto& event) {
      return event.type == "tool/result" &&
             event.data.value("status", "") == "outcome_unknown";
    }));
    snow::TrajectoryValidator{}.validate(repaired).throw_if_invalid();
  }

  {
    std::atomic_int active{0};
    std::atomic_int maximum{0};
    snow::ModelResponse parallel_calls;
    parallel_calls.finish_reason = "tool_calls";
    parallel_calls.tool_calls = {
        {tokmon::ToolCallId("parallel-a"), "parallel_probe", {{"delay_ms", 80}}},
        {tokmon::ToolCallId("parallel-b"), "parallel_probe", {{"delay_ms", 20}}}};
    snow::ModelResponse done;
    done.content = "parallel complete";
    auto parallel_model = std::make_shared<snow::ScriptedModelProvider>(
        std::vector<snow::ModelResponse>{parallel_calls, done});
    snow::BootstrapConfig parallel_config = config;
    parallel_config.data_root = temporary / "parallel-data";
    snow::Assembly parallel_assembly(parallel_config, parallel_model);
    auto registry = parallel_assembly.arche_runtime().root_context()->require<
        snow::ToolRegistry>("snow.tools", "^1.0");
    registry->add(std::make_shared<ParallelProbeTool>(active, maximum));
    const auto parallel_session = parallel_assembly.agent().create_session();
    const auto parallel_result = parallel_assembly.agent().run(
        parallel_session, "probe", {.model = "fake"});
    assert(parallel_result.reason == snow::TurnEndReason::completed);
    assert(maximum.load() == 2);
    std::vector<std::string> result_order;
    for (const auto& event : parallel_assembly.agent().events(parallel_session)) {
      if (event.type == "tool/result")
        result_order.push_back(event.tool_call_id->str());
    }
    assert((result_order == std::vector<std::string>{"parallel-a", "parallel-b"}));
  }

  {
    constexpr std::string_view secret = "sk-super-secret-value";
    auto redaction_model = std::make_shared<snow::ScriptedModelProvider>(
        std::vector<snow::ModelResponse>{
            {.content = "echo sk-super-secret-value"}});
    snow::BootstrapConfig redaction_config = config;
    redaction_config.data_root = temporary / "redaction-data";
    redaction_config.sensitive_values = {std::string(secret)};
    snow::Assembly redaction_assembly(redaction_config, redaction_model);
    const auto redaction_session =
        redaction_assembly.agent().create_session();
    const auto redacted = redaction_assembly.agent().run(
        redaction_session, "input sk-super-secret-value");
    assert(redacted.final_text == "echo [REDACTED]");
    for (const auto& redaction_event :
         redaction_assembly.agent().events(redaction_session))
      assert(redaction_event.data.dump().find(secret) == std::string::npos);
  }

  std::cout << "snow_tests: ok\n";
  return 0;
}
