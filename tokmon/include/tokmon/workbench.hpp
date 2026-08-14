#pragma once

#include <tokmon/approval.hpp>
#include <tokmon/projection.hpp>
#include <tokmon/settings.hpp>

#include <white/document.hpp>
#include <white/renderer.hpp>

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tokmon::desktop {

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
  std::optional<PendingApproval> approval;
  std::string session_id;
  std::string status;
  std::string message_input;
  std::string file_filter;
  std::string model;
  std::string trajectory_search;
  DesktopSettings settings;
  std::string active_settings_field;
  std::vector<WorkbenchSession> sessions;
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
  bool trajectory_search_focused{false};
  bool settings_field_focused{false};
  bool caret_visible{true};
  bool window_maximized{false};
};

struct WorkbenchLayout {
  white::Rect menu_bar;
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
  bool compact_sidebar{false};
  bool viewer_visible{false};
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
  explicit WorkbenchView(std::filesystem::path workspace);

  [[nodiscard]] WorkbenchLayout layout(float width, float height) const;
  void draw(white::RasterSurface &surface, const WorkbenchFrame &frame);
  [[nodiscard]] WorkbenchAction dispatch(const white::UiEvent &event);
  [[nodiscard]] const std::filesystem::path &
  selected_document() const noexcept {
    return selected_document_;
  }
  bool show_document(const std::filesystem::path &path);
  void close_settings() noexcept { settings_open_ = false; }

private:
  struct FileEntry {
    std::filesystem::path relative;
    std::string label;
    bool directory{false};
    std::size_t depth{0};
  };
  struct HitTarget {
    white::Rect bounds;
    WorkbenchActionKind action{WorkbenchActionKind::none};
    std::filesystem::path file;
    std::string value;
    std::size_t index{0};
    bool directory{false};
    bool close_tab{false};
  };

  void refresh_files(std::string_view filter = {});
  void open_document(const std::filesystem::path &relative);
  void close_document(const std::filesystem::path &relative);
  [[nodiscard]] std::size_t editor_offset_at(float x, float y,
                                             const white::Rect &bounds,
                                             std::string_view text) const;
  [[nodiscard]] bool hovered(const white::Rect &bounds) const noexcept;

  std::filesystem::path workspace_;
  std::filesystem::path selected_document_;
  std::vector<FileEntry> files_;
  std::vector<std::filesystem::path> open_documents_;
  std::set<std::filesystem::path> expanded_directories_;
  std::vector<std::string> document_lines_;
  std::vector<HitTarget> hits_;
  WorkbenchLayout last_layout_;
  float timeline_scroll_{0};
  float timeline_max_scroll_{0};
  float document_scroll_{0};
  float document_max_scroll_{0};
  float session_scroll_{0};
  float session_max_scroll_{0};
  float pointer_x_{-1};
  float pointer_y_{-1};
  float sidebar_width_{224};
  float viewer_width_{0};
  std::size_t previous_item_count_{0};
  std::size_t previous_trajectory_event_count_{0};
  std::string active_menu_;
  std::string settings_tab_{"general"};
  std::string trajectory_filter_{"all"};
  std::set<std::uint64_t> expanded_trajectory_events_;
  float trajectory_scroll_{0};
  float trajectory_max_scroll_{0};
  bool profile_menu_open_{false};
  bool settings_open_{false};
  bool trajectory_open_{false};
  bool follow_tail_{true};
  bool sidebar_collapsed_{false};
  bool viewer_collapsed_{false};
  bool sidebar_manually_sized_{false};
  bool viewer_manually_sized_{false};
  bool resizing_sidebar_{false};
  bool resizing_viewer_{false};
  bool selecting_input_{false};
  white::Rect message_editor_bounds_;
  white::Rect filter_editor_bounds_;
  white::Rect settings_editor_bounds_;
  white::Rect trajectory_search_bounds_;
  white::Rect open_menu_bounds_;
  white::Rect profile_menu_bounds_;
  white::Rect settings_modal_bounds_;
  std::string message_editor_text_;
  std::string filter_editor_text_;
  std::string settings_editor_text_;
  std::string settings_editor_field_;
  std::string trajectory_search_text_;
  bool selecting_filter_{false};
  std::string selecting_editor_;
  std::string last_filter_;
};

} // namespace tokmon::desktop
