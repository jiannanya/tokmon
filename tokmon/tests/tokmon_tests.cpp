#include <tokmon/approval.hpp>
#include <tokmon/app.hpp>
#include <tokmon/common/digest.hpp>
#include <tokmon/common/files.hpp>
#include <tokmon/product_assembly.hpp>
#include <tokmon/projection.hpp>
#include <tokmon/settings.hpp>
#include <tokmon/snow_client.hpp>
#include <tokmon/workbench.hpp>
#include <tokmon/workbench_document.hpp>
#include <white/assembly.hpp>

#include <cassert>
#include <cmath>
#include <future>
#include <iostream>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

int main() {
  assert(tokmon::desktop::WorkbenchDocument::html_source().contains(
      "data-native=\"tokmon.conversation\""));
  assert(tokmon::desktop::WorkbenchDocument::css_source().contains(
      "#workbench-body"));
  tokmon::desktop::Projection projection;
  const tokmon::SessionId session("session");
  auto event = [&](std::uint64_t seq, std::string type, tokmon::Json data) {
    snow::TrajectoryEvent value;
    value.seq = seq;
    value.type = std::move(type);
    value.session_id = session;
    value.turn_id = tokmon::TurnId("turn");
    value.time = "2026-08-14T14:54:00.000Z";
    value.data = std::move(data);
    return value;
  };
  projection.apply(event(1, "user/message", {{"content", "hello"}}));
  projection.apply(event(2, "assistant/chunk", {{"content", "hel"}}));
  projection.apply(event(3, "assistant/chunk", {{"content", "lo"}}));
  projection.apply(
      event(4, "assistant/message",
            {{"content", "hello"}, {"tool_calls", tokmon::Json::array()}}));
  auto snapshot = projection.snapshot();
  assert(snapshot.size() == 2);
  assert(snapshot[1].content == "hello");
  assert(snapshot[1].status == "committed");
  assert(projection.cursor() == 4);
  projection.apply(
      event(5, "turn/end", {{"reason", "completed"}, {"elapsed_ms", 2450}}));
  snapshot = projection.snapshot();
  assert(snapshot[1].metadata["elapsed_ms"] == 2450);
  assert(snapshot[0].metadata["time"] == "2026-08-14T14:54:00.000Z");
  projection.apply(event(6, "plugin.example/custom",
                         {{"value", 42}, {"message", "fallback"}}));
  snapshot = projection.snapshot();
  assert(snapshot.size() == 3);
  assert(snapshot.back().title == "Event / plugin.example/custom");
  projection.apply(event(7, "session/header", {{"composition_epoch", 1}}));
  projection.apply(event(8, "session/end", {{"reason", "closed"}}));
  assert(projection.snapshot().size() == 3);
  assert(projection.event_snapshot().size() == 8);
  assert(projection.event_snapshot().back().type == "session/end");

  {
    const auto settings_root = std::filesystem::temp_directory_path() /
                               ("tokmon-settings-test-" + tokmon::make_uuid());
    std::filesystem::create_directories(settings_root / ".custom");
    tokmon::write_text_file_atomic(
        settings_root / ".custom" / "tokmon.json",
        tokmon::Json{{"schema", "org.tokmon.desktop.config/v1"},
                     {"ui", {{"scale", 1.4}}},
                     {"extension_value", 42}}
            .dump(2));
    auto settings =
        tokmon::desktop::load_desktop_settings(settings_root, ".custom");
    settings.provider_id = "acme_gateway";
    settings.provider_name = "Acme Gateway";
    settings.endpoint = "https://gateway.example/v1/chat/completions";
    settings.api_key_env = "ACME_API_KEY";
    settings.model = "acme-reasoner";
    settings.theme = "dark";
    settings.raw_trace = true;
    settings.max_steps = "64";
    settings.default_permission = "allow";
    settings.plugins.push_back(
        {"optional-memory", "org.example.memory@1.0.0", "memory", true, false});
    tokmon::desktop::save_desktop_settings(settings_root, settings);
    const auto loaded =
        tokmon::desktop::load_desktop_settings(settings_root, ".custom");
    assert(loaded.config_dir_name == ".custom");
    assert(loaded.provider_id == "acme_gateway");
    assert(loaded.provider_name == "Acme Gateway");
    assert(loaded.endpoint == settings.endpoint);
    assert(loaded.api_key_env == "ACME_API_KEY");
    assert(loaded.model == "acme-reasoner");
    assert(loaded.theme == "dark");
    assert(loaded.raw_trace);
    assert(loaded.max_steps == "64");
    assert(loaded.default_permission == "allow");
    assert(loaded.plugins.back().disabled);
    const auto product = tokmon::Json::parse(
        tokmon::read_text_file(settings_root / ".custom" / "tokmon.json"));
    assert(product.at("extension_value") == 42);
    const auto app_config =
        tokmon::desktop::load_app_config(settings_root, ".custom");
    assert(std::abs(app_config.ui_scale - 1.4F) < 0.001F);
    const auto providers = tokmon::Json::parse(
        tokmon::read_text_file(settings_root / ".custom" / "providers.json"));
    assert(providers.at("selected") == "acme_gateway");
    assert(providers.at("default").at("endpoint") == settings.endpoint);
    assert(!providers.at("default").contains("api_key"));
    assert(std::filesystem::exists(settings_root / ".custom" /
                                   "composition.json"));
    assert(std::filesystem::exists(settings_root / ".custom" / "policy.json"));
    std::filesystem::remove_all(settings_root);
  }

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
    auto composed_projection = std::make_shared<tokmon::desktop::Projection>();
    auto composed_approvals =
        std::make_shared<tokmon::desktop::ApprovalCoordinator>();
    auto composed_workbench = std::make_shared<tokmon::desktop::WorkbenchView>(
        std::filesystem::temp_directory_path(),
        composed_white.native_components_shared());
    tokmon::desktop::ProductAssembly product(
        composed_runtime, composed_projection, composed_approvals, {},
        composed_workbench);
    assert(product.report().actions.size() == 3);
    assert(composed_runtime.fiber("white.runtime"));
    assert(composed_runtime.fiber("tokmon.projection"));
    assert(composed_runtime.fiber("tokmon.workbench"));
    assert(composed_white.native_components().contains("tokmon.sidebar"));
    assert(composed_runtime.fibers().size() == 12);
  }

  {
    const auto ui_root = std::filesystem::temp_directory_path() /
                         ("tokmon-workbench-test-" + tokmon::make_uuid());
    std::filesystem::create_directories(ui_root / "docs");
    tokmon::write_text_file_atomic(
        ui_root / "README.md",
        "# Tokmon\n\nArche Agent OS 工作台\n\n```json\n{\"ok\":true}\n```\n");
    tokmon::desktop::WorkbenchView workbench(ui_root);
    const auto desktop_layout = workbench.layout(1500, 900);
    assert(desktop_layout.viewer_visible);
    assert(desktop_layout.sidebar_visible);
    assert(std::abs(desktop_layout.sidebar.width - 192.0F) < 1.0F);
    assert(std::abs(desktop_layout.conversation.x -
                    desktop_layout.sidebar.width) < 0.1F);
    assert(desktop_layout.conversation.width >= 500);
    assert(desktop_layout.document.width > 0);
    assert(desktop_layout.viewer.x >=
           desktop_layout.conversation.x + desktop_layout.conversation.width);
    assert(desktop_layout.viewer.x + desktop_layout.viewer.width <= 1500);
    assert(desktop_layout.composer.x > desktop_layout.conversation.x);
    assert(desktop_layout.composer.x < desktop_layout.conversation.x + 30.0F);
    const auto narrow_layout = workbench.layout(800, 700);
    assert(!narrow_layout.viewer_visible);
    assert(narrow_layout.sidebar_visible);
    assert(std::abs(narrow_layout.sidebar.width - 192.0F) < 1.0F);
    assert(narrow_layout.conversation.x > 191.0F);
    assert(narrow_layout.conversation.width >= 600);
    const auto compact_layout = workbench.layout(640, 700);
    assert(!compact_layout.viewer_visible);
    assert(!compact_layout.sidebar_visible);
    assert(compact_layout.conversation.x == 0);
    assert(compact_layout.conversation.width >= 600);
    assert(compact_layout.composer.width < compact_layout.conversation.width);

    white::RasterSurface workbench_surface(1500, 900);
    tokmon::desktop::WorkbenchFrame frame;
    frame.items = snapshot;
    frame.trajectory_events = projection.event_snapshot();
    frame.settings = tokmon::desktop::load_desktop_settings(ui_root);
    frame.items[1].content =
        "已完成 Tokmon 对话流。\n\n主要能力：\n\n- **用户消息**使用右侧气泡。\n"
        "- 助手正文支持 `Shift+Enter`、[设计文档](docs/design.md) 和项目符号。";
    frame.items[1].metadata["elapsed_ms"] = 2450;
    frame.session_id = "session";
    frame.status = "Snow connected";
    frame.message_input = "hello world";
    frame.model = "gpt-5";
    frame.snow_connected = true;
    frame.message_focused = true;
    frame.caret_visible = true;
    frame.editor_cursor = 5;
    frame.selection_start = 5;
    frame.selection_end = 5;
    // The UI must preserve this order. The current session deliberately sits
    // in the middle and is highlighted in place instead of being promoted.
    frame.sessions = {{"alpha-session", "Alpha session", "1", 4, false},
                      {"session", "Current session name", "2", 8, false},
                      {"omega-session", "Omega session", "3", 12, false}};
    frame.trajectory_cursor = projection.cursor();
    frame.composition_epoch = 7;
    workbench.draw(workbench_surface, frame);
    assert(workbench.drag_region().has_value());
    workbench_surface.reconfigure(800, 700);
    workbench.draw(workbench_surface, frame);
    assert(workbench.layout(static_cast<float>(workbench_surface.width()),
                            static_cast<float>(workbench_surface.height())) ==
           narrow_layout);
    assert(workbench_surface.pixels() != nullptr);
    workbench_surface.reconfigure(1500, 900);
    workbench.draw(workbench_surface, frame);
    assert(workbench.selected_document() == std::filesystem::path("README.md"));
    const auto surface_hash = [&] {
      const auto *bytes =
          static_cast<const unsigned char *>(workbench_surface.pixels());
      const auto size = workbench_surface.row_bytes() *
                        static_cast<std::size_t>(workbench_surface.height());
      std::uint64_t hash = 1469598103934665603ULL;
      for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
      }
      return hash;
    };
    const auto assert_hover_changes = [&](std::string_view control,
                                          const auto &hover_frame, float x,
                                          float y) {
      auto hover_action = workbench.dispatch({"pointermove", -10, -10});
      assert(hover_action.kind == tokmon::desktop::WorkbenchActionKind::none ||
             hover_action.kind ==
                 tokmon::desktop::WorkbenchActionKind::redraw);
      workbench.draw(workbench_surface, hover_frame);
      const auto normal_hash = surface_hash();
      hover_action = workbench.dispatch({"pointermove", x, y});
      if (hover_action.kind != tokmon::desktop::WorkbenchActionKind::redraw)
        std::cerr << "Missing hover target for " << control << " at " << x
                  << ',' << y << '\n';
      assert(hover_action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
      workbench_surface.begin_frame();
      workbench.draw(workbench_surface, hover_frame);
      const auto hover_damage = workbench_surface.frame_damage();
      assert(!hover_damage.empty());
      assert(!hover_damage.full());
      if (surface_hash() == normal_hash)
        std::cerr << "Missing hover pixels for " << control << " at " << x
                  << ',' << y << '\n';
      assert(surface_hash() != normal_hash);
      hover_action = workbench.dispatch({"pointermove", x, y});
      assert(hover_action.kind == tokmon::desktop::WorkbenchActionKind::none);
      hover_action = workbench.dispatch({.type = "pointerleave"});
      assert(hover_action.kind ==
             tokmon::desktop::WorkbenchActionKind::redraw);
      workbench.draw(workbench_surface, hover_frame);
      hover_action = workbench.dispatch({.type = "pointerleave"});
      assert(hover_action.kind == tokmon::desktop::WorkbenchActionKind::none);
    };
    // User bubble copy affordance sits under the first message bubble.
    const float user_copy_x = 1095;
    const float user_copy_y = 154;
    assert_hover_changes("window-minimize", frame, 1424, 18);
    assert_hover_changes("message-copy", frame, user_copy_x, user_copy_y);
    assert_hover_changes("send", frame, 953, 873);
    assert_hover_changes("viewer-preview-tab", frame, 1300, 23);
    assert_hover_changes("viewer-file-menu", frame, 1200, 61);
    auto attachment_frame = frame;
    attachment_frame.attachments.push_back({"note.txt", 12});
    assert_hover_changes("attachment-remove", attachment_frame, 462, 790);
    auto approval_frame = frame;
    approval_frame.approval = tokmon::desktop::PendingApproval{
        "approval",
        {"write_file", "write", tokmon::Json::object()},
        {{"path", "note.txt"}},
        "需要写入工作区",
        {}};
    const auto approval_width =
        std::min(420.0F, desktop_layout.conversation.width - 60);
    const auto approval_x =
        desktop_layout.conversation.x +
        (desktop_layout.conversation.width - approval_width) / 2;
    assert_hover_changes("approval-primary", approval_frame,
                         approval_x + approval_width - 48,
                         desktop_layout.conversation.y + 277);
    auto action = workbench.dispatch({"click", user_copy_x, user_copy_y});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::copy_text);
    assert(action.value == "hello");
    action = workbench.dispatch({"click", 120, 90});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::new_session);
    action = workbench.dispatch({"click", 100, 114});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::focus_filter);
    action = workbench.dispatch({"wheel", desktop_layout.timeline.x + 10,
                                 desktop_layout.timeline.y + 10, 0, 48});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::none);
    const float composer_center_x =
        desktop_layout.conversation.x + desktop_layout.conversation.width / 2;
    action = workbench.dispatch({"pointerdown", composer_center_x,
                                 desktop_layout.composer.y + 20});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::focus_message);
    assert(action.cursor <= frame.message_input.size());
    action = workbench.dispatch({"pointermove", composer_center_x + 50,
                                 desktop_layout.composer.y + 20});
    assert(action.kind ==
           tokmon::desktop::WorkbenchActionKind::set_editor_cursor);
    assert(action.extend_selection);
    (void)workbench.dispatch({"click", composer_center_x + 50,
                              desktop_layout.composer.y + 20});
    action = workbench.dispatch({"click", 387, 873});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::attach_files);
    // The Group->Project->Session tree binds the first project's sessions to
    // the committed session list; the first session row is Alpha even though
    // the active session is the second item.
    action = workbench.dispatch({"click", 100, 216});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::switch_session);
    assert(action.value == "alpha-session");

    // Both side panels collapse and expand from persistent header controls.
    action = workbench.dispatch({"click", 174, 21});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    assert(workbench.layout(1500, 900).sidebar.width == 0);
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 18, 18});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    assert(std::abs(workbench.layout(1500, 900).sidebar.width - 192.0F) <
           1.0F);

    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 1130, 18});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    assert(!workbench.layout(1500, 900).viewer_visible);
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 1392, 18});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    assert(workbench.layout(1500, 900).viewer_visible);

    // Splitters update layout continuously while pointer movement is active.
    auto resized_layout = workbench.layout(1500, 900);
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch(
        {"pointerdown", resized_layout.sidebar_splitter.x + 3, 400});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    assert(action.pointer_cursor && *action.pointer_cursor);
    action = workbench.dispatch({"pointermove", 280, 400});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    resized_layout = workbench.layout(1500, 900);
    assert(resized_layout.sidebar.width == 280);
    action = workbench.dispatch({"click", 280, 400});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::none);

    workbench.draw(workbench_surface, frame);
    resized_layout = workbench.layout(1500, 900);
    action = workbench.dispatch(
        {"pointerdown", resized_layout.viewer_splitter.x + 3, 400});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    assert(action.pointer_cursor && *action.pointer_cursor);
    action = workbench.dispatch({"pointermove", 1000, 400});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    resized_layout = workbench.layout(1500, 900);
    assert(std::abs(resized_layout.viewer.width - 500) < 0.1F);
    action = workbench.dispatch({"click", 1000, 400});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::none);

    // The floating window controls expose real native-window actions.
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 1424, 18});
    assert(action.kind ==
           tokmon::desktop::WorkbenchActionKind::window_minimize);
    action = workbench.dispatch({"click", 1453, 18});
    assert(action.kind ==
           tokmon::desktop::WorkbenchActionKind::window_toggle_maximize);
    action = workbench.dispatch({"click", 1482, 18});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::window_close);

    // The trajectory tab is backed by raw Snow events. Search, filtering,
    // selection and export are real interaction targets.
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 353, 65});
    assert(action.kind ==
           tokmon::desktop::WorkbenchActionKind::show_trajectory);
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"pointerdown", 934, 118});
    assert(action.kind ==
           tokmon::desktop::WorkbenchActionKind::focus_trajectory_search);
    action = workbench.dispatch({"click", 400, 340});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    action = workbench.dispatch({"click", 616, 270});
    assert(action.kind ==
           tokmon::desktop::WorkbenchActionKind::set_trajectory_filter);
    assert(action.value == "turns");
    action = workbench.dispatch({"click", 674, 270});
    assert(action.kind ==
           tokmon::desktop::WorkbenchActionKind::export_trajectory);
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"wheel", 460, 300, 0, 60});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::none);
    action = workbench.dispatch({"click", 285, 65});
    assert(action.kind ==
           tokmon::desktop::WorkbenchActionKind::show_conversation);

    // The sidebar settings entry opens the modal with navigable sections,
    // editable fields, toggles and a durable save command.
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 120, 877});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::open_settings);
    workbench.draw(workbench_surface, frame);
    assert_hover_changes("settings-nav-models", frame, 350, 265);
    action = workbench.dispatch({"click", 350, 265});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::settings_tab);
    assert(action.value == "models");
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 800, 330});
    assert(action.kind ==
           tokmon::desktop::WorkbenchActionKind::focus_settings_field);
    assert(action.value == "provider_id");
    frame.active_settings_field = "provider_id";
    frame.settings_field_focused = true;
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"pointerdown", 800, 330});
    assert(action.kind ==
           tokmon::desktop::WorkbenchActionKind::focus_settings_field);
    action = workbench.dispatch({"click", 800, 330});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::none);
    action = workbench.dispatch({"click", 350, 232});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::settings_tab);
    assert(action.value == "general");
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 800, 238});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::set_setting);
    assert(action.value.starts_with("language="));
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 1135, 715});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::save_settings);

    // A long conversation stays scrollable with a working tail affordance.
    workbench.close_settings();
    workbench.draw(workbench_surface, frame);
    auto long_frame = frame;
    long_frame.items[1].content.clear();
    for (int line = 0; line < 40; ++line)
      long_frame.items[1].content +=
          "- Long assistant output keeps the conversation scrollable.\n";
    workbench.draw(workbench_surface, long_frame);
    const auto active_layout = workbench.layout(1500, 900);
    assert_hover_changes(
        "scroll-to-tail", long_frame,
        active_layout.timeline.x + active_layout.timeline.width / 2,
        active_layout.composer.y - 15);
    action = workbench.dispatch(
        {"click", active_layout.timeline.x + active_layout.timeline.width / 2,
         active_layout.composer.y - 15});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    tokmon::desktop::WorkbenchFrame empty_frame;
    empty_frame.session_id = "empty";
    empty_frame.message_focused = true;
    workbench.draw(workbench_surface, empty_frame);
    assert_hover_changes("suggestion", empty_frame,
                         active_layout.timeline.x +
                             active_layout.timeline.width / 2,
                         active_layout.timeline.y + 180);
    action = workbench.dispatch(
        {"click", active_layout.timeline.x + active_layout.timeline.width / 2,
         active_layout.timeline.y + 180});
    assert(action.kind ==
           tokmon::desktop::WorkbenchActionKind::set_message_input);
    assert(!action.value.empty());

    // The composer dropdowns pick access levels and models; the file dropdown
    // in the code inspector switches demo documents.
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 400, 868});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 400, 787});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 750, 868});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 750, 764});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::set_setting);
    assert(action.value.starts_with("model="));
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 875, 868});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 875, 793});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 648, 868});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    action = workbench.dispatch({"click", 1107, 61});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);
    workbench.draw(workbench_surface, frame);
    action = workbench.dispatch({"click", 1104, 140});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::redraw);

    // The project tree caps visible recent sessions and therefore never steals
    // the conversation wheel when a workspace contains many archived sessions.
    auto many_sessions = frame;
    many_sessions.sessions.clear();
    for (int index = 0; index < 30; ++index)
      many_sessions.sessions.push_back(
          {"session-" + std::to_string(index),
           "Named session " + std::to_string(index), std::to_string(index),
           static_cast<std::uint64_t>(index), false});
    workbench.draw(workbench_surface, many_sessions);
    action = workbench.dispatch({"wheel", 40, 400, 0, 96});
    assert(action.kind == tokmon::desktop::WorkbenchActionKind::none);
    assert(workbench.show_document(ui_root / "README.md"));
    assert(!workbench.show_document(std::filesystem::temp_directory_path() /
                                    "outside.md"));
    std::filesystem::remove_all(ui_root);
  }

#ifdef TOKMON_TEST_SNOW_EXE
  const auto process_root = std::filesystem::temp_directory_path() /
                            ("tokmon-process-test-" + tokmon::make_uuid());
  std::filesystem::create_directories(process_root / ".third-party");
  tokmon::write_text_file_atomic(
      process_root / ".third-party" / "tokmon.json",
      tokmon::Json{{"schema", "org.tokmon.desktop.config/v1"},
                   {"snow", {{"mode", "process"}}}}
          .dump(2));
  tokmon::desktop::SnowProcessClient client(
      {.executable = TOKMON_TEST_SNOW_EXE,
       .workspace = process_root,
       .data_root = process_root / ".third-party" / "data",
       .config_dir_name = ".third-party",
       .request_timeout = std::chrono::seconds(10)});
  std::promise<tokmon::desktop::SnowProcessExit> crashed;
  std::atomic_bool crash_delivered{false};
  client.set_crash_handler([&](const tokmon::desktop::SnowProcessExit &value) {
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
  const auto listed = client.request("session.list", {{"limit", 10}});
  assert(listed.is_array());
  assert(std::ranges::any_of(listed, [&](const auto &item) {
    return item.value("session_id", "") == child_session;
  }));
  const tokmon::Json process_attachment = {
      {"name", "context.txt"},
      {"content", "process attachment"},
      {"sha256", tokmon::sha256_hex("process attachment")}};
  const tokmon::Json turn_parameters = {
      {"session_id", child_session},
      {"message", "hello"},
      {"attachments", tokmon::Json::array({process_attachment})}};
  const auto turn = client.request("turn.start", turn_parameters);
  assert(turn.at("reason") == "completed");
  const auto child_events = client.request(
      "session.events", {{"session_id", child_session}, {"after", 0}});
  assert(child_events.is_array());
  assert(!child_events.empty());
  assert(std::ranges::any_of(child_events, [](const auto &event) {
    return event.value("type", "") == "user/message" &&
           event["data"].value("attachments", tokmon::Json::array()).size() ==
               1;
  }));
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
