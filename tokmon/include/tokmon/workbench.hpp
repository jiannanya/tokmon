#pragma once

#include <tokmon/approval.hpp>
#include <tokmon/projection.hpp>
#include <tokmon/settings.hpp>

#include <white/document.hpp>
#include <white/renderer.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace white {
class NativeComponentRegistry;
}

namespace tokmon::desktop {

class WorkbenchDocument;

struct WorkbenchSession {
  std::string id;
  std::string title;
  std::string created_at;
  std::uint64_t last_seq{0};
  bool closed{false};
};

struct WorkbenchAttachment {
  std::string name;
  std::uint64_t bytes{0};
};

struct WorkbenchFrame {
  std::vector<ConversationItem> items;
  std::vector<snow::TrajectoryEvent> trajectory_events;
  const std::vector<ConversationItem> *item_source{nullptr};
  const std::vector<snow::TrajectoryEvent> *trajectory_event_source{nullptr};
  std::optional<PendingApproval> approval;
  std::string session_id;
  std::string status;
  std::string message_input;
  std::string file_filter;
  std::string rename_draft;
  std::string model;
  std::string trajectory_search;
  DesktopSettings settings;
  std::string active_settings_field;
  std::vector<WorkbenchSession> sessions;
  const std::vector<WorkbenchSession> *session_source{nullptr};
  std::vector<WorkbenchAttachment> attachments;
  std::size_t editor_cursor{0};
  std::size_t selection_start{0};
  std::size_t selection_end{0};
  std::uint64_t trajectory_cursor{0};
  std::uint64_t composition_epoch{0};
  bool snow_connected{false};
  bool turn_active{false};
  bool message_focused{true};
  bool filter_focused{false};
  bool rename_active{false};
  bool rename_focused{false};
  bool trajectory_search_focused{false};
  bool settings_field_focused{false};
  bool caret_visible{true};
  bool window_maximized{false};

  [[nodiscard]] const std::vector<ConversationItem> &conversation_items()
      const noexcept {
    return item_source ? *item_source : items;
  }
  [[nodiscard]] const std::vector<snow::TrajectoryEvent> &events()
      const noexcept {
    return trajectory_event_source ? *trajectory_event_source
                                   : trajectory_events;
  }
  [[nodiscard]] const std::vector<WorkbenchSession> &session_items()
      const noexcept {
    return session_source ? *session_source : sessions;
  }
};

// Layout regions for the flush Tokmon UI 3.0 shell (docs/Tokmon UI): a
// resizable left navigation rail, the main conversation/trajectory column,
// and the right code-inspector panel. Window chrome floats above everything.
struct WorkbenchLayout {
  white::Rect bounds;
  white::Rect sidebar;
  white::Rect conversation;
  white::Rect conversation_header;
  white::Rect timeline;
  white::Rect composer;
  white::Rect viewer;
  white::Rect viewer_header;
  white::Rect document;
  white::Rect explorer;
  white::Rect sidebar_splitter;
  white::Rect viewer_splitter;
  bool sidebar_visible{false};
  bool viewer_visible{false};
  friend bool operator==(const WorkbenchLayout&,
                         const WorkbenchLayout&) = default;
};

enum class WorkbenchActionKind {
  none,
  redraw,
  new_session,
  fork_session,
  diagnostics,
  inspect_composition,
  cancel_turn,
  submit_input,
  set_message_input,
  copy_text,
  scroll_to_tail,
  focus_message,
  focus_filter,
  focus_rename,
  set_editor_cursor,
  switch_session,
  attach_files,
  open_file_dialog,
  remove_attachment,
  show_help,
  approve,
  deny,
  toggle_menu,
  toggle_left_panel,
  toggle_right_panel,
  window_minimize,
  window_toggle_maximize,
  window_close,
  open_settings,
  close_settings,
  open_archive,
  close_archive,
  open_plugins,
  close_plugins,
  save_settings,
  open_config_file,
  focus_settings_field,
  set_setting,
  focus_trajectory_search,
  export_trajectory,
  show_conversation,
  show_trajectory,
  set_trajectory_filter,
  toggle_trajectory_event,
  toggle_profile_menu,
  settings_tab,
  viewer_tab,
};

struct WorkbenchAction {
  WorkbenchActionKind kind{WorkbenchActionKind::none};
  std::string value;
  std::size_t index{0};
  std::size_t cursor{0};
  bool extend_selection{false};
  std::optional<bool> pointer_cursor;
};

// Product-level White view. It owns only ephemeral presentation state; Snow's
// durable trajectory remains the canonical Agent state.
class WorkbenchView final {
public:
  // Logical-width threshold below which the workspace viewer is force-hidden.
  // Must stay at or below the default window's logical width
  // (window_width / ui_scale = 1500 / 1.25 = 1200) so the viewer — and its
  // expand affordance — remain reachable at the shipped default geometry.
  // 880 logical units corresponds to roughly 1100 design pixels at the
  // shipped scale, matching the minimum width at which the Figma three-column
  // layout (240 + 400 + 440 plus splitters) remains usable.
  static constexpr int viewer_visible_breakpoint = 880;

  explicit WorkbenchView(
      std::filesystem::path workspace,
      std::shared_ptr<white::NativeComponentRegistry> native_components = {});
  ~WorkbenchView();

  [[nodiscard]] WorkbenchLayout layout(float width, float height) const;
  void draw(white::RasterSurface &surface, const WorkbenchFrame &frame);
  [[nodiscard]] WorkbenchAction dispatch(const white::UiEvent &event);
  [[nodiscard]] const std::filesystem::path &
  selected_document() const noexcept {
    return selected_document_;
  }
  bool show_document(const std::filesystem::path &path);
  void set_viewer_collapsed(bool collapsed) noexcept { viewer_collapsed_ = collapsed; }
  void close_settings() noexcept { settings_open_ = false; }
  // Quiet span of the main header that the borderless window treats as its
  // native title-bar drag region (logical UI units, recomputed each draw).
  [[nodiscard]] const std::optional<white::Rect> &
  drag_region() const noexcept {
    return drag_region_;
  }
  // Headless capture helpers: drive presentation-only state so a single
  // rendered frame can exercise the settings modal or trajectory inspector.
  void open_settings_preset(std::string tab) {
    settings_open_ = true;
    if (!tab.empty()) settings_tab_ = std::move(tab);
  }
  void set_trajectory_open(bool open) noexcept { trajectory_open_ = open; }

private:
  struct HitTarget {
    white::Rect bounds;
    WorkbenchActionKind action{WorkbenchActionKind::none};
    std::filesystem::path file;
    std::string value;
    std::size_t index{0};
    bool directory{false};
    bool close_tab{false};
  };

  void open_document(const std::filesystem::path &relative);
  void close_document(const std::filesystem::path &relative);
  [[nodiscard]] std::size_t editor_offset_at(float x, float y,
                                             const white::Rect &bounds,
                                             std::string_view text) const;
  [[nodiscard]] bool hovered(const white::Rect &bounds) noexcept;
  [[nodiscard]] std::optional<std::size_t>
  hover_region_at(float x, float y) const noexcept;
  void request_redraw(white::Rect damage = {}) noexcept;

  std::filesystem::path workspace_;
  std::unique_ptr<WorkbenchDocument> shell_;
  std::filesystem::path selected_document_;
  std::vector<std::filesystem::path> open_documents_;
  std::vector<std::string> document_lines_;
  std::vector<HitTarget> hits_;
  std::vector<white::Rect> hover_regions_;
  std::optional<std::size_t> active_hover_region_;
  std::optional<white::Rect> pending_damage_;
  WorkbenchLayout last_layout_;
  float timeline_scroll_{0};
  float timeline_max_scroll_{0};
  float document_scroll_{0};
  float document_max_scroll_{0};
  float trajectory_scroll_{0};
  float trajectory_max_scroll_{0};
  float pointer_x_{-1};
  float pointer_y_{-1};
  // White uses logical units; 192 maps to the Figma Make sidebar's 240 px
  // default width at the shipped 1.25 UI scale.
  float sidebar_width_{192};
  float viewer_width_{0};
  std::size_t previous_item_count_{0};
  std::string settings_tab_{"general"};
  std::string trajectory_filter_{"all"};
  bool settings_open_{false};
  bool trajectory_open_{false};
  bool follow_tail_{true};
  bool sidebar_collapsed_{false};
  bool viewer_collapsed_{false};
  bool sidebar_manually_sized_{false};
  bool viewer_manually_sized_{false};
  bool resizing_sidebar_{false};
  bool resizing_viewer_{false};
  bool pointer_cursor_active_{false};
  bool selecting_input_{false};
  // Presentation-only state for the Tokmon UI shell.
  std::set<std::string> tree_collapsed_{"g1/音频切片处理", "g3"};
  bool workflow_expanded_{true};
  // 0 = closed, 1 = access, 2 = model, 3 = reasoning, 4 = context.
  int composer_menu_{0};
  std::string selected_access_{"完全访问"};
  std::string selected_reasoning_{"最高"};
  std::size_t selected_trajectory_event_{1};
  int trajectory_detail_tab_{0};
  bool viewer_file_menu_{false};
  std::string viewer_demo_file_{"transcribe.py"};
  white::Rect message_editor_bounds_;
  white::Rect filter_editor_bounds_;
  white::Rect rename_editor_bounds_;
  white::Rect settings_editor_bounds_;
  white::Rect trajectory_search_bounds_;
  white::Rect settings_modal_bounds_;
  white::Rect composer_menu_bounds_;
  white::Rect viewer_menu_bounds_;
  std::optional<white::Rect> drag_region_;
  std::string message_editor_text_;
  std::string filter_editor_text_;
  std::string rename_editor_text_;
  std::string settings_editor_text_;
  std::string settings_editor_field_;
  std::string trajectory_search_text_;
  std::string viewer_tab_{"code"};
  std::size_t editor_cursor_{0};
  bool selecting_filter_{false};
  std::string selecting_editor_;
  std::string last_filter_;
  std::size_t last_frame_key_{0};
  bool last_caret_visible_{true};
  bool has_frame_{false};
  bool full_redraw_pending_{true};
};

} // namespace tokmon::desktop
