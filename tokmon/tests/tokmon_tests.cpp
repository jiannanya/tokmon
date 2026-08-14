#include <tokmon/approval.hpp>
#include <tokmon/projection.hpp>
#include <tokmon/snow_client.hpp>
#include <tokmon/product_assembly.hpp>
#include <tokmon/common/files.hpp>
#include <white/assembly.hpp>

#include <cassert>
#include <future>
#include <iostream>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

int main() {
  tokmon::desktop::Projection projection;
  const tokmon::SessionId session("session");
  auto event = [&](std::uint64_t seq, std::string type,
                   tokmon::Json data) {
    snow::TrajectoryEvent value;
    value.seq = seq;
    value.type = std::move(type);
    value.session_id = session;
    value.data = std::move(data);
    return value;
  };
  projection.apply(event(1, "user/message", {{"content", "hello"}}));
  projection.apply(event(2, "assistant/chunk", {{"content", "hel"}}));
  projection.apply(event(3, "assistant/chunk", {{"content", "lo"}}));
  projection.apply(event(4, "assistant/message",
                         {{"content", "hello"}, {"tool_calls", tokmon::Json::array()}}));
  auto snapshot = projection.snapshot();
  assert(snapshot.size() == 2);
  assert(snapshot[1].content == "hello");
  assert(snapshot[1].status == "committed");
  assert(projection.cursor() == 4);
  projection.apply(event(5, "plugin.example/custom",
                         {{"value", 42}, {"message", "fallback"}}));
  snapshot = projection.snapshot();
  assert(snapshot.size() == 3);
  assert(snapshot.back().title == "Event / plugin.example/custom");

  auto approvals = std::make_shared<tokmon::desktop::ApprovalCoordinator>();
  snow::ToolDefinition tool{"write_file", "write", tokmon::Json::object()};
  auto future = std::async(std::launch::async, [&] {
    return approvals->approve(tool, {{"path", "a.txt"}}, "mutable");
  });
  while (!approvals->pending()) {
    std::this_thread::yield();
  }
  assert(approvals->resolve(true));
  assert(future.get());

  auto remote = std::make_shared<tokmon::desktop::ApprovalCoordinator>();
  std::promise<std::pair<std::string, bool>> remote_response;
  remote->present({"approval-1", tool, {{"path", "remote.txt"}}, "mutable"},
                  [&](std::string id, bool approved) {
                    remote_response.set_value({std::move(id), approved});
                  });
  assert(remote->resolve(false));
  const auto remote_value = remote_response.get_future().get();
  assert(remote_value.first == "approval-1");
  assert(!remote_value.second);

  {
    arche::Runtime composed_runtime("tokmon-composition-test");
    white::Assembly composed_white(composed_runtime);
    auto composed_projection =
        std::make_shared<tokmon::desktop::Projection>();
    auto composed_approvals =
        std::make_shared<tokmon::desktop::ApprovalCoordinator>();
    tokmon::desktop::ProductAssembly product(
        composed_runtime, composed_projection, composed_approvals);
    assert(product.report().actions.size() == 2);
    assert(composed_runtime.fiber("white.runtime"));
    assert(composed_runtime.fiber("tokmon.projection"));
    assert(composed_runtime.fibers().size() == 7);
  }

#ifdef TOKMON_TEST_SNOW_EXE
  const auto process_root =
      std::filesystem::temp_directory_path() /
      ("tokmon-process-test-" + tokmon::make_uuid());
  std::filesystem::create_directories(process_root / ".third-party");
  tokmon::write_text_file_atomic(
      process_root / ".third-party" / "tokmon.json",
      tokmon::Json{{"schema", "org.tokmon.desktop.config/v1"},
                   {"snow", {{"mode", "process"}}}}.dump(2));
  tokmon::desktop::SnowProcessClient client({
      .executable = TOKMON_TEST_SNOW_EXE,
      .workspace = process_root,
      .data_root = process_root / ".third-party" / "data",
      .config_dir_name = ".third-party",
      .request_timeout = std::chrono::seconds(10)});
  std::promise<tokmon::desktop::SnowProcessExit> crashed;
  std::atomic_bool crash_delivered{false};
  client.set_crash_handler(
      [&](const tokmon::desktop::SnowProcessExit& value) {
        if (!value.expected && !crash_delivered.exchange(true))
          crashed.set_value(value);
      });
  auto crash_future = crashed.get_future();
  client.start();
  assert(client.alive());
  assert(client.process_id() != 0);
  const auto initialized = client.initialize();
  assert(initialized.at("selected_protocol") == 1);
  const auto created = client.request("session.create");
  const auto child_session = created.at("session_id").get<std::string>();
  const auto turn = client.request(
      "turn.start", {{"session_id", child_session}, {"message", "hello"}});
  assert(turn.at("reason") == "completed");
  const auto child_events = client.request(
      "session.events", {{"session_id", child_session}, {"after", 0}});
  assert(child_events.is_array());
  assert(!child_events.empty());
  const auto cursor = child_events.back().at("seq").get<std::uint64_t>();

#ifdef _WIN32
  const auto child = OpenProcess(PROCESS_TERMINATE, FALSE, client.process_id());
  assert(child != nullptr);
  assert(TerminateProcess(child, 73));
  CloseHandle(child);
  assert(crash_future.wait_for(std::chrono::seconds(5)) ==
         std::future_status::ready);
  const auto exit = crash_future.get();
  assert(!exit.expected);
  client.stop();
  client.start();
  (void)client.initialize();
  const auto resumed = client.request(
      "session.resume", {{"session_id", child_session}, {"after", cursor}});
  assert(resumed.at("session_id") == child_session);
#endif
  client.stop();
  std::filesystem::remove_all(process_root);
#endif

  std::cout << "tokmon_tests: ok\n";
  return 0;
}
