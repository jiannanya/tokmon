#include <tokmon/workbench.hpp>

#include <tokmon/common/files.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>

namespace tokmon::desktop {
namespace {

using white::Color;
using white::Rect;
using white::RasterSurface;

// White's product skin intentionally uses a very small warm-neutral palette.
// Hierarchy comes from spacing and typography; borders are reserved for actual
// pane boundaries and interactive surfaces.
constexpr Color canvas{245, 245, 243, 255};
constexpr Color sidebar_background{241, 241, 239, 255};
constexpr Color panel{253, 253, 252, 255};
constexpr Color ink{29, 30, 33, 255};
constexpr Color secondary{91, 93, 100, 255};
constexpr Color muted{143, 145, 151, 255};
constexpr Color hairline{225, 225, 222, 255};
constexpr Color hover_fill{231, 231, 228, 255};
constexpr Color hover_border{200, 201, 198, 255};
constexpr Color selected_fill{224, 224, 221, 255};
constexpr Color accent{54, 92, 205, 255};
constexpr Color accent_hover{43, 77, 180, 255};
constexpr Color success{20, 154, 74, 255};
constexpr Color warning{238, 92, 34, 255};
constexpr Color danger{210, 48, 54, 255};

float label(RasterSurface& surface, std::string_view value, const Rect& rect,
            float size = 13, Color color = ink, int weight = 400,
            std::size_t lines = 1,
            white::TextAlign align = white::TextAlign::left,
            bool monospace = false, float line_height = 1.28F) {
  return surface.paragraph(value, rect, size, color, weight, line_height,
                           lines, align, monospace);
}

std::size_t utf8_length(std::string_view value) {
  std::size_t count = 0;
  for (const auto character : value) {
    if ((static_cast<unsigned char>(character) & 0xc0U) != 0x80U) ++count;
  }
  return count;
}

float visual_units(std::string_view value) {
  float result = 0;
  for (std::size_t offset = 0; offset < value.size();) {
    const auto first = static_cast<unsigned char>(value[offset]);
    const std::size_t width = first < 0x80U   ? 1
                              : first < 0xe0U ? 2
                              : first < 0xf0U ? 3
                                               : 4;
    result += first < 0x80U ? (value[offset] == ' ' ? 0.55F : 1.0F) : 1.85F;
    offset = std::min(value.size(), offset + width);
  }
  return result;
}

std::string utf8_prefix(std::string_view value, std::size_t limit) {
  std::size_t count = 0;
  std::size_t end = 0;
  while (end < value.size() && count < limit) {
    const auto first = static_cast<unsigned char>(value[end]);
    const std::size_t width = first < 0x80U   ? 1
                              : first < 0xe0U ? 2
                              : first < 0xf0U ? 3
                                               : 4;
    end = std::min(value.size(), end + width);
    ++count;
  }
  auto result = std::string(value.substr(0, end));
  if (end < value.size()) result += "…";
  return result;
}

std::vector<std::string> split_lines(std::string_view value) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find('\n', start);
    auto line = std::string(value.substr(
        start, end == std::string_view::npos ? value.size() - start
                                             : end - start));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    result.push_back(std::move(line));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return result;
}

std::string markdown_inline(std::string value) {
  const auto erase_all = [&](std::string_view marker) {
    for (auto position = value.find(marker); position != std::string::npos;
         position = value.find(marker, position))
      value.erase(position, marker.size());
  };
  erase_all("**");
  erase_all("__");
  erase_all("`");
  return value;
}

std::vector<white::RichTextSpan> inline_markdown_spans(
    std::string_view value, float size = 14.0F, Color color = ink,
    int weight = 400) {
  std::vector<white::RichTextSpan> result;
  const auto append = [&](std::string_view text, int span_weight,
                          bool monospace = false,
                          std::optional<Color> background = {},
                          Color span_color = ink) {
    if (text.empty()) return;
    result.push_back({std::string(text), size, span_color, span_weight,
                      monospace, background});
  };
  std::size_t cursor = 0;
  while (cursor < value.size()) {
    if (value.substr(cursor).starts_with("**")) {
      const auto end = value.find("**", cursor + 2);
      if (end != std::string_view::npos) {
        append(value.substr(cursor + 2, end - cursor - 2), 650, false, {},
               color);
        cursor = end + 2;
        continue;
      }
    }
    if (value[cursor] == '`') {
      const auto end = value.find('`', cursor + 1);
      if (end != std::string_view::npos) {
        append(value.substr(cursor + 1, end - cursor - 1), 500, true,
               Color{235, 236, 238, 255}, color);
        cursor = end + 1;
        continue;
      }
    }
    if (value[cursor] == '[') {
      const auto label_end = value.find(']', cursor + 1);
      if (label_end != std::string_view::npos &&
          label_end + 1 < value.size() && value[label_end + 1] == '(') {
        const auto target_end = value.find(')', label_end + 2);
        if (target_end != std::string_view::npos) {
          append(value.substr(cursor + 1, label_end - cursor - 1), 600,
                 false, {}, accent);
          cursor = target_end + 1;
          continue;
        }
      }
    }
    auto next = cursor + 1;
    while (next < value.size() && value[next] != '`' && value[next] != '[' &&
           !value.substr(next).starts_with("**"))
      ++next;
    append(value.substr(cursor, next - cursor), weight, false, {}, color);
    cursor = next;
  }
  if (result.empty()) append(value, weight, false, {}, color);
  return result;
}

std::size_t estimated_rows(std::string_view value, float width, float size,
                           float reserved = 0) {
  const auto columns = std::max(
      12.0F, std::max(40.0F, width - reserved) /
                 std::max(6.0F, size * 0.58F));
  return std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(
                                      visual_units(markdown_inline(
                                          std::string(value))) /
                                      columns)));
}

float markdown_height(std::string_view content, float width) {
  float height = 0;
  bool code = false;
  for (auto line : split_lines(content)) {
    if (line.starts_with("```")) {
      code = !code;
      height += 6;
      continue;
    }
    if (line.empty()) {
      height += 11;
      continue;
    }
    if (code) {
      height += static_cast<float>(estimated_rows(line, width, 12, 22)) *
                    20.0F +
                3;
      continue;
    }
    if (line.starts_with("# ")) {
      height += static_cast<float>(estimated_rows(line.substr(2), width, 20)) *
                    31.0F +
                7;
    } else if (line.starts_with("## ")) {
      height += static_cast<float>(estimated_rows(line.substr(3), width, 17)) *
                    27.0F +
                6;
    } else if (line.starts_with("### ")) {
      height += static_cast<float>(estimated_rows(line.substr(4), width, 15)) *
                    24.0F +
                5;
    } else {
      const bool bullet = line.starts_with("- ") || line.starts_with("* ") ||
                          (line.size() > 2 && std::isdigit(
                                                   static_cast<unsigned char>(
                                                       line.front())) &&
                           line.find(". ") < 4);
      const auto text = bullet ? line.substr(line.find(' ') + 1) : line;
      height += static_cast<float>(estimated_rows(text, width, 14,
                                                  bullet ? 24.0F : 0.0F)) *
                    22.0F +
                (bullet ? 5.0F : 7.0F);
    }
  }
  return std::max(26.0F, height);
}

float draw_markdown(RasterSurface& surface, std::string_view content,
                    float x, float y, float width) {
  const auto start_y = y;
  bool code = false;
  for (auto line : split_lines(content)) {
    if (line.starts_with("```")) {
      code = !code;
      y += 6;
      continue;
    }
    if (line.empty()) {
      y += 11;
      continue;
    }
    if (code) {
      const auto rows = estimated_rows(line, width, 12, 22);
      const auto block_height = static_cast<float>(rows) * 20.0F + 3;
      surface.fill_rect({x, y, width, block_height},
                        {246, 247, 248, 255}, 5);
      label(surface, line, {x + 11, y + 2, width - 22, block_height - 2},
            12, {61, 64, 70, 255}, 400, rows, white::TextAlign::left, true,
            1.5F);
      y += block_height;
      continue;
    }
    float size = 14;
    int weight = 400;
    float after = 7;
    if (line.starts_with("# ")) {
      line.erase(0, 2);
      size = 20;
      weight = 700;
      after = 7;
    } else if (line.starts_with("## ")) {
      line.erase(0, 3);
      size = 17;
      weight = 680;
      after = 6;
    } else if (line.starts_with("### ")) {
      line.erase(0, 4);
      size = 15;
      weight = 650;
      after = 5;
    }
    bool bullet = false;
    std::string bullet_label;
    if (line.starts_with("- ") || line.starts_with("* ")) {
      bullet = true;
      bullet_label = "•";
      line.erase(0, 2);
    } else if (line.size() > 2 &&
               std::isdigit(static_cast<unsigned char>(line.front()))) {
      const auto marker = line.find(". ");
      if (marker != std::string::npos && marker < 4) {
        bullet = true;
        bullet_label = line.substr(0, marker + 1);
        line.erase(0, marker + 2);
      }
    }
    const float text_x = x + (bullet ? 22.0F : 0.0F);
    if (bullet)
      label(surface, bullet_label, {x + 1, y + 1, 16, 22}, size, ink, 550,
            1, white::TextAlign::center);
    const auto spans = inline_markdown_spans(line, size, ink, weight);
    const auto extent = surface.rich_paragraph(
        spans, {text_x, y, width - (text_x - x), 1000}, 1.52F);
    y += std::max(size * 1.52F, extent) + after;
  }
  return y - start_y;
}

std::string clock_label(const tokmon::Json& metadata) {
  const auto value = metadata.value("time", metadata.value("completed_at", ""));
  if (value.size() < 16) return "";
  std::tm utc{};
  std::istringstream parser(value.substr(0, 16));
  parser >> std::get_time(&utc, "%Y-%m-%dT%H:%M");
  if (parser.fail()) return value.substr(11, 5);
#ifdef _WIN32
  const auto timestamp = _mkgmtime64(&utc);
  std::tm local{};
  if (timestamp < 0 || localtime_s(&local, &timestamp) != 0)
    return value.substr(11, 5);
#else
  const auto timestamp = timegm(&utc);
  std::tm local{};
  if (timestamp < 0 || !localtime_r(&timestamp, &local))
    return value.substr(11, 5);
#endif
  std::ostringstream formatted;
  formatted << std::setfill('0') << std::setw(2) << local.tm_hour << ':'
            << std::setw(2) << local.tm_min;
  return formatted.str();
}

std::string elapsed_label(const ConversationItem& item) {
  if (item.status == "streaming") return "正在生成";
  if (!item.metadata.contains("elapsed_ms")) return "已完成";
  const auto millis = std::max<std::int64_t>(
      0, item.metadata.value("elapsed_ms", std::int64_t{0}));
  const auto seconds = (millis + 999) / 1000;
  const auto minutes = seconds / 60;
  const auto remainder = seconds % 60;
  if (minutes > 0)
    return "耗时 " + std::to_string(minutes) + "分 " +
           std::to_string(remainder) + "秒";
  return "耗时 " + std::to_string(remainder) + "秒";
}

std::string normalized_title(const WorkbenchFrame& frame) {
  for (const auto& item : frame.items) {
    if (item.kind != ItemKind::user || item.content.empty()) continue;
    auto title = item.content;
    std::ranges::replace(title, '\n', ' ');
    return utf8_prefix(title, 30);
  }
  return "新建 Arche Agent 会话";
}

float item_height(const ConversationItem& item, float width) {
  const auto columns = std::max<std::size_t>(
      28, static_cast<std::size_t>(width / 8.2F));
  const auto visual_lines = std::max<std::size_t>(
      1, (utf8_length(item.content) + columns - 1) / columns);
  switch (item.kind) {
  case ItemKind::user:
    return 40 + static_cast<float>(visual_lines) * 21;
  case ItemKind::assistant:
    return 45 + markdown_height(item.content, width - 28);
  case ItemKind::tool:
  case ItemKind::artifact:
  case ItemKind::diagnostic:
    return 68 + static_cast<float>(std::min<std::size_t>(visual_lines, 5)) *
                    18;
  default:
    return 50 + static_cast<float>(std::min<std::size_t>(visual_lines, 3)) *
                    18;
  }
}

bool is_text_file(const std::filesystem::path& path) {
  const auto extension = path.extension().string();
  static constexpr std::string_view extensions[] = {
      ".md",   ".txt",  ".json", ".cpp", ".cc", ".c",   ".hpp",
      ".h",    ".cmake", ".mjs", ".js",   ".ts", ".py", ".css"};
  return path.filename() == "CMakeLists.txt" ||
         std::ranges::find(extensions, extension) != std::end(extensions);
}

void draw_icon(RasterSurface& surface, std::string_view name, float x, float y,
               Color color) {
  if (name == "plus") {
    surface.line(x - 5, y, x + 5, y, color, 1.5F);
    surface.line(x, y - 5, x, y + 5, color, 1.5F);
  } else if (name == "chat") {
    surface.stroke_rect({x - 7, y - 6, 14, 11}, color, 1.3F, 3);
    surface.line(x - 3, y + 5, x - 6, y + 8, color, 1.3F);
  } else if (name == "branch") {
    // Tokmon brand mark, kept identical to the supplied reference.
    surface.fill_circle(x - 4, y - 6, 2, color);
    surface.fill_circle(x + 5, y, 2, color);
    surface.fill_circle(x - 4, y + 6, 2, color);
    surface.line(x - 4, y - 4, x - 4, y + 4, color, 1.2F);
    surface.line(x - 2, y, x + 3, y, color, 1.2F);
  } else if (name == "fork") {
    // A duplicated session card and transfer arrow is intentionally unrelated
    // to Tokmon's three-node brand mark.
    surface.stroke_rect({x - 7, y - 7, 9, 10}, color, 1.2F, 2);
    surface.stroke_rect({x + 1, y - 2, 8, 9}, color, 1.2F, 2);
    surface.line(x - 4, y + 6, x + 4, y + 6, color, 1.25F);
    surface.line(x + 4, y + 6, x + 1, y + 3, color, 1.25F);
  } else if (name == "panel-left") {
    surface.stroke_rect({x - 7, y - 6, 14, 12}, color, 1.2F, 2);
    surface.line(x - 2, y - 5, x - 2, y + 5, color, 1.2F);
  } else if (name == "panel-right") {
    surface.stroke_rect({x - 7, y - 6, 14, 12}, color, 1.2F, 2);
    surface.line(x + 2, y - 5, x + 2, y + 5, color, 1.2F);
  } else if (name == "window-minimize") {
    surface.line(x - 5, y + 3, x + 5, y + 3, color, 1.2F);
  } else if (name == "window-maximize") {
    surface.stroke_rect({x - 5, y - 5, 10, 10}, color, 1.15F, 1);
  } else if (name == "window-restore") {
    surface.stroke_rect({x - 3, y - 5, 8, 8}, color, 1.1F, 1);
    surface.stroke_rect({x - 5, y - 3, 8, 8}, color, 1.1F, 1);
  } else if (name == "window-close") {
    surface.line(x - 4, y - 4, x + 4, y + 4, color, 1.2F);
    surface.line(x + 4, y - 4, x - 4, y + 4, color, 1.2F);
  } else if (name == "plugin") {
    surface.stroke_rect({x - 5, y - 5, 10, 10}, color, 1.2F, 2);
    surface.line(x - 2, y - 8, x - 2, y - 5, color, 1.2F);
    surface.line(x + 2, y - 8, x + 2, y - 5, color, 1.2F);
    surface.line(x - 2, y + 5, x - 2, y + 8, color, 1.2F);
    surface.line(x + 2, y + 5, x + 2, y + 8, color, 1.2F);
  } else if (name == "pulse") {
    surface.stroke_rect({x - 7, y - 7, 14, 14}, color, 1.2F, 7);
    surface.line(x - 5, y, x - 2, y, color, 1.2F);
    surface.line(x - 2, y, x, y - 4, color, 1.2F);
    surface.line(x, y - 4, x + 2, y + 4, color, 1.2F);
    surface.line(x + 2, y + 4, x + 5, y, color, 1.2F);
  } else if (name == "folder") {
    surface.stroke_rect({x - 7, y - 5, 14, 10}, color, 1.2F, 2);
    surface.line(x - 6, y - 5, x - 2, y - 8, color, 1.2F);
    surface.line(x - 2, y - 8, x + 2, y - 8, color, 1.2F);
    surface.line(x + 2, y - 8, x + 4, y - 5, color, 1.2F);
  } else if (name == "file") {
    surface.stroke_rect({x - 6, y - 8, 12, 16}, color, 1.1F, 2);
    surface.line(x - 3, y - 3, x + 3, y - 3, color, 1.0F);
    surface.line(x - 3, y + 1, x + 3, y + 1, color, 1.0F);
  } else if (name == "search") {
    surface.stroke_rect({x - 6, y - 6, 10, 10}, color, 1.3F, 5);
    surface.line(x + 3, y + 3, x + 8, y + 8, color, 1.3F);
  } else if (name == "send") {
    surface.line(x - 5, y + 5, x + 5, y, color, 1.7F);
    surface.line(x + 5, y, x - 5, y - 5, color, 1.7F);
  } else if (name == "stop") {
    surface.fill_rect({x - 4, y - 4, 8, 8}, color, 2);
  } else if (name == "copy") {
    surface.stroke_rect({x - 5, y - 6, 9, 10}, color, 1.1F, 2);
    surface.stroke_rect({x - 2, y - 3, 9, 10}, color, 1.1F, 2);
  } else if (name == "edit") {
    surface.line(x - 5, y + 5, x + 4, y - 4, color, 1.4F);
    surface.line(x + 3, y - 5, x + 6, y - 2, color, 1.4F);
    surface.line(x - 6, y + 6, x - 2, y + 5, color, 1.2F);
  } else if (name == "down") {
    surface.line(x - 5, y - 2, x, y + 3, color, 1.4F);
    surface.line(x, y + 3, x + 5, y - 2, color, 1.4F);
  } else if (name == "chevron") {
    surface.line(x - 3, y - 5, x + 2, y, color, 1.2F);
    surface.line(x + 2, y, x - 3, y + 5, color, 1.2F);
  } else {
    surface.fill_circle(x, y, 2, color);
  }
}

void draw_editor_text(RasterSurface& surface, std::string_view text,
                      const Rect& bounds, std::size_t cursor,
                      std::size_t selection_start,
                      std::size_t selection_end, bool focused,
                      bool caret_visible, float font_size = 13.0F,
                      std::size_t max_lines = 2) {
  constexpr float row_height = 18.0F;
  float x = bounds.x;
  float y = bounds.y;
  float caret_x = x;
  float caret_y = y;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto first = static_cast<unsigned char>(text[offset]);
    const std::size_t bytes = first < 0x80U   ? 1
                              : first < 0xe0U ? 2
                              : first < 0xf0U ? 3
                                               : 4;
    if (offset == cursor) {
      caret_x = x;
      caret_y = y;
    }
    if (text[offset] == '\n') {
      x = bounds.x;
      y += row_height;
      offset += bytes;
      continue;
    }
    const auto advance = first < 0x80U ? (text[offset] == ' ' ? 4.2F : 7.2F)
                                       : 13.0F;
    if (x + advance > bounds.x + bounds.width) {
      x = bounds.x;
      y += row_height;
      if (offset == cursor) {
        caret_x = x;
        caret_y = y;
      }
    }
    if (offset < selection_end && offset + bytes > selection_start)
      surface.fill_rect({x - 1, y, advance + 2, row_height},
                        {196, 219, 250, 220}, 2);
    x += advance;
    offset = std::min(text.size(), offset + bytes);
  }
  if (cursor >= text.size()) {
    caret_x = x;
    caret_y = y;
  }
  label(surface, text, bounds, font_size, ink, 400, max_lines,
        white::TextAlign::left, false, 1.38F);
  if (focused && caret_visible)
    surface.line(caret_x, caret_y + 1, caret_x, caret_y + row_height - 1,
                 {38, 92, 190, 255}, 1.4F);
}

} // namespace

WorkbenchView::WorkbenchView(std::filesystem::path workspace)
    : workspace_(std::filesystem::weakly_canonical(std::move(workspace))) {
  refresh_files();
  if (std::filesystem::exists(workspace_ / "README.md")) {
    open_document("README.md");
  } else {
    selected_document_ = "Welcome";
    document_lines_ = {"# Tokmon", "", "Arche Agent OS 工作台已就绪。",
                       "在左侧创建会话，在中间与 Snow 协作。"};
  }
}

WorkbenchLayout WorkbenchView::layout(float width, float height) const {
  WorkbenchLayout result;
  result.menu_bar = {0, 0, width, 44};
  result.compact_sidebar = width < 980 && !sidebar_manually_sized_;
  const auto sidebar_content_reserve =
      width >= 1160 && !viewer_collapsed_ ? 820.0F : 560.0F;
  const auto expanded_sidebar =
      std::clamp(sidebar_width_, 176.0F,
                 std::max(176.0F, width - sidebar_content_reserve));
  const float sidebar_width = sidebar_collapsed_
                                  ? 0.0F
                                  : (result.compact_sidebar ? 72.0F
                                                            : expanded_sidebar);
  result.sidebar = {0, 44, sidebar_width, std::max(0.0F, height - 44)};
  if (sidebar_width > 0)
    result.sidebar_splitter = {sidebar_width - 3, 44, 6,
                               std::max(0.0F, height - 44)};
  const float body_x = sidebar_width;
  const float body_y = 44;
  const float body_height = std::max(200.0F, height - body_y);
  const float available = std::max(320.0F, width - body_x);
  result.viewer_visible = width >= 1160 && !viewer_collapsed_;
  float conversation_width = available;
  if (result.viewer_visible) {
    if (viewer_manually_sized_) {
      const auto max_viewer = std::max(320.0F, available - 500.0F);
      const auto actual_viewer =
          std::clamp(viewer_width_, 320.0F, max_viewer);
      conversation_width = available - actual_viewer;
    } else {
      conversation_width =
          std::clamp(available * 0.49F, 560.0F, 720.0F);
    }
  }
  result.conversation = {body_x, body_y, conversation_width, body_height};
  result.conversation_header = {body_x, body_y, conversation_width, 58};
  result.composer = {body_x + 28, body_y + body_height - 112,
                     conversation_width - 56, 88};
  result.timeline = {body_x + 1, body_y + 59, conversation_width - 2,
                     std::max(50.0F, result.composer.y - body_y - 72)};
  if (result.viewer_visible) {
    result.viewer = {body_x + conversation_width, body_y,
                     available - conversation_width, body_height};
    result.viewer_splitter = {result.viewer.x - 3, body_y, 6, body_height};
    result.viewer_header = {result.viewer.x, result.viewer.y,
                            result.viewer.width, 92};
    const float explorer_width =
        std::clamp(result.viewer.width * 0.29F, 196.0F, 248.0F);
    result.document = {result.viewer.x + 1, result.viewer.y + 93,
                       result.viewer.width - explorer_width - 2,
                       result.viewer.height - 94};
    result.explorer = {result.document.x + result.document.width,
                       result.document.y, explorer_width,
                       result.document.height};
  }
  return result;
}

void WorkbenchView::refresh_files(std::string_view filter) {
  files_.clear();
  const auto lowered = [](std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    return value;
  };
  const auto query = lowered(std::string(filter));
  std::function<void(const std::filesystem::path&, std::size_t)> visit;
  visit = [&](const std::filesystem::path& directory, std::size_t depth) {
    if (depth > 8 || files_.size() >= 1000) return;
    std::error_code error;
    std::vector<std::filesystem::directory_entry> entries;
    for (const auto& entry :
         std::filesystem::directory_iterator(directory, error)) {
      if (error) break;
      if (entry.is_symlink(error)) continue;
      entries.push_back(entry);
    }
    std::ranges::sort(entries, [](const auto& left, const auto& right) {
      std::error_code left_error;
      std::error_code right_error;
      const auto left_directory = left.is_directory(left_error);
      const auto right_directory = right.is_directory(right_error);
      if (left_directory != right_directory) return left_directory;
      return left.path().filename().string() <
             right.path().filename().string();
    });
    for (const auto& entry : entries) {
      if (files_.size() >= 1000) break;
      const auto relative = std::filesystem::relative(entry.path(), workspace_);
      const auto name = entry.path().filename().string();
      if (name == ".git" || name == "build" || name == "vcpkg_installed")
        continue;
      const auto directory_entry = entry.is_directory(error);
      if (!directory_entry && !is_text_file(entry.path())) continue;
      const auto matches =
          query.empty() || lowered(relative.generic_string()).contains(query);
      if (query.empty() || matches || directory_entry)
        files_.push_back({relative, name, directory_entry, depth});
      if (directory_entry &&
          (!query.empty() || expanded_directories_.contains(relative)))
        visit(entry.path(), depth + 1);
    }
  };
  visit(workspace_, 0);
  if (!query.empty()) {
    std::erase_if(files_, [&](const auto& entry) {
      return entry.directory &&
             !lowered(entry.relative.generic_string()).contains(query) &&
             std::ranges::none_of(files_, [&](const auto& child) {
               const auto parent = child.relative.parent_path();
               return !child.directory &&
                      (parent == entry.relative ||
                       parent.generic_string().starts_with(
                           entry.relative.generic_string() + "/")) &&
                      lowered(child.relative.generic_string()).contains(query);
             });
    });
  }
}

void WorkbenchView::open_document(const std::filesystem::path& relative) {
  try {
    const auto path = tokmon::canonical_within(workspace_, relative, true);
    if (!std::filesystem::is_regular_file(path) || !is_text_file(path)) return;
    if (std::filesystem::file_size(path) > 1024U * 1024U) {
      selected_document_ = relative;
      document_lines_ = {"# 文件过大", "", "预览限制为 1 MiB。"};
      document_scroll_ = 0;
      return;
    }
    selected_document_ = std::filesystem::relative(path, workspace_);
    if (std::ranges::find(open_documents_, selected_document_) ==
        open_documents_.end())
      open_documents_.push_back(selected_document_);
    document_lines_ = split_lines(tokmon::read_text_file(path));
    document_scroll_ = 0;
  } catch (const std::exception& error) {
    selected_document_ = relative;
    document_lines_ = {"# 无法预览", "", error.what()};
    document_scroll_ = 0;
  }
}

bool WorkbenchView::show_document(const std::filesystem::path& path) {
  try {
    const auto canonical = tokmon::canonical_within(workspace_, path, true);
    if (!std::filesystem::is_regular_file(canonical) ||
        !is_text_file(canonical))
      return false;
    open_document(std::filesystem::relative(canonical, workspace_));
    return selected_document_ ==
           std::filesystem::relative(canonical, workspace_);
  } catch (...) {
    return false;
  }
}

void WorkbenchView::close_document(const std::filesystem::path& relative) {
  const auto found = std::ranges::find(open_documents_, relative);
  if (found == open_documents_.end()) return;
  const auto index = static_cast<std::size_t>(
      std::distance(open_documents_.begin(), found));
  open_documents_.erase(found);
  if (selected_document_ != relative) return;
  if (open_documents_.empty()) {
    selected_document_ = "Welcome";
    document_lines_ = {"# Tokmon", "", "选择工作区文件以开始预览。"};
  } else {
    open_document(open_documents_[std::min(index, open_documents_.size() - 1)]);
  }
}

std::size_t WorkbenchView::editor_offset_at(
    float x, float y, const Rect& editor_bounds,
    std::string_view editor_text) const {
  constexpr float row_height = 18.0F;
  auto cursor_x = editor_bounds.x;
  auto cursor_y = editor_bounds.y;
  std::size_t offset = 0;
  std::size_t nearest = 0;
  float nearest_distance = std::numeric_limits<float>::max();
  while (offset <= editor_text.size()) {
    const auto distance = std::abs(cursor_y - y) * 4.0F +
                          std::abs(cursor_x - x);
    if (distance < nearest_distance) {
      nearest_distance = distance;
      nearest = offset;
    }
    if (offset == editor_text.size()) break;
    const auto first = static_cast<unsigned char>(editor_text[offset]);
    const std::size_t width = first < 0x80U   ? 1
                              : first < 0xe0U ? 2
                              : first < 0xf0U ? 3
                                               : 4;
    if (editor_text[offset] == '\n') {
      cursor_x = editor_bounds.x;
      cursor_y += row_height;
    } else {
      const auto advance = first < 0x80U ? (editor_text[offset] == ' ' ? 4.2F
                                                                            : 7.2F)
                                         : 13.0F;
      if (cursor_x + advance > editor_bounds.x + editor_bounds.width) {
        cursor_x = editor_bounds.x;
        cursor_y += row_height;
      }
      cursor_x += advance;
    }
    offset = std::min(editor_text.size(), offset + width);
  }
  return nearest;
}

bool WorkbenchView::hovered(const Rect& bounds) const noexcept {
  return bounds.contains(pointer_x_, pointer_y_);
}

void WorkbenchView::draw(RasterSurface& surface,
                         const WorkbenchFrame& frame) {
  const float width = static_cast<float>(surface.width());
  const float height = static_cast<float>(surface.height());
  last_layout_ = layout(width, height);
  hits_.clear();
  open_menu_bounds_ = {};
  if (frame.file_filter != last_filter_) {
    last_filter_ = frame.file_filter;
    refresh_files(last_filter_);
  }
  surface.clear(canvas);

  const auto add_hit = [&](Rect bounds, WorkbenchActionKind action,
                           std::filesystem::path file = {}) {
    hits_.push_back({bounds, action, std::move(file)});
  };
  const auto button = [&](Rect bounds, std::string_view text,
                          WorkbenchActionKind action, bool primary = false,
                          Color text_color = ink) {
    const auto is_hovered = hovered(bounds);
    const auto background = primary
                                ? (is_hovered ? Color{64, 67, 73, 255}
                                              : Color{44, 46, 50, 255})
                                : (is_hovered ? hover_fill : panel);
    surface.fill_rect(bounds, background, bounds.height * 0.45F);
    surface.stroke_rect(bounds,
                        primary ? background
                                : (is_hovered ? hover_border : hairline),
                        1,
                        bounds.height * 0.45F);
    label(surface, text, {bounds.x + 8, bounds.y + 6, bounds.width - 16,
                          bounds.height - 8},
          12, primary ? Color{255, 255, 255, 255} : text_color, 500, 1,
          white::TextAlign::center);
    add_hit(bounds, action);
  };

  // Quiet application bar: one brand mark, low-contrast commands and a single
  // runtime signal. The bar anchors the workspace without competing with it.
  surface.fill_rect(last_layout_.menu_bar, panel);
  surface.line(0, 43.5F, width, 43.5F, hairline);
  draw_icon(surface, "branch", 19, 22, secondary);
  label(surface, "Tokmon", {37, 12, 58, 22}, 12, ink, 650);
  constexpr Rect menu_items[] = {{102, 7, 46, 30}, {152, 7, 46, 30},
                                 {202, 7, 46, 30}, {252, 7, 46, 30}};
  constexpr std::string_view menu_ids[] = {"file", "edit", "view", "help"};
  for (std::size_t index = 0; index < std::size(menu_items); ++index) {
    if (hovered(menu_items[index]) || active_menu_ == menu_ids[index])
      surface.fill_rect(menu_items[index], hover_fill, 6);
  }
  label(surface, "文件", {112, 13, 38, 22}, 11, secondary, 450);
  label(surface, "编辑", {162, 13, 38, 22}, 11, secondary, 450);
  label(surface, "视图", {212, 13, 38, 22}, 11, secondary, 450);
  label(surface, "帮助", {262, 13, 38, 22}, 11, secondary, 450);
  for (std::size_t index = 0; index < std::size(menu_items); ++index)
    hits_.push_back({menu_items[index], WorkbenchActionKind::toggle_menu, {},
                     std::string(menu_ids[index])});
  const Rect toggle_left{310, 7, 30, 30};
  if (hovered(toggle_left)) surface.fill_rect(toggle_left, hover_fill, 7);
  draw_icon(surface, "panel-left", toggle_left.x + 15, toggle_left.y + 15,
            sidebar_collapsed_ ? accent : secondary);
  add_hit(toggle_left, WorkbenchActionKind::toggle_left_panel);
  const Rect toggle_right{width - 310, 7, 30, 30};
  if (hovered(toggle_right)) surface.fill_rect(toggle_right, hover_fill, 7);
  draw_icon(surface, "panel-right", toggle_right.x + 15,
            toggle_right.y + 15,
            viewer_collapsed_ ? accent : secondary);
  add_hit(toggle_right, WorkbenchActionKind::toggle_right_panel);
  const auto runtime_text = frame.snow_connected ? "Snow 已连接" : "Snow 离线";
  surface.fill_circle(width - 260, 22, 3.5F,
                      frame.snow_connected ? success : danger);
  label(surface, runtime_text, {width - 250, 13, 112, 20}, 10, secondary, 500);
  const Rect minimize{width - 126, 0, 42, 43};
  const Rect maximize{width - 84, 0, 42, 43};
  const Rect window_close_bounds{width - 42, 0, 42, 43};
  if (hovered(minimize)) surface.fill_rect(minimize, hover_fill);
  if (hovered(maximize)) surface.fill_rect(maximize, hover_fill);
  if (hovered(window_close_bounds))
    surface.fill_rect(window_close_bounds, {225, 75, 75, 255});
  draw_icon(surface, "window-minimize", minimize.x + 21, 21, secondary);
  draw_icon(surface, frame.window_maximized ? "window-restore"
                                            : "window-maximize",
            maximize.x + 21, 21, secondary);
  draw_icon(surface, "window-close", window_close_bounds.x + 21, 21,
            hovered(window_close_bounds) ? Color{255, 255, 255, 255}
                                         : secondary);
  add_hit(minimize, WorkbenchActionKind::window_minimize);
  add_hit(maximize, WorkbenchActionKind::window_toggle_maximize);
  add_hit(window_close_bounds, WorkbenchActionKind::window_close);

  // Sidebar: one primary action, three destinations and recent context.
  if (last_layout_.sidebar.width > 0) {
  surface.fill_rect(last_layout_.sidebar, sidebar_background);
  surface.line(last_layout_.sidebar.width - 0.5F, 44,
               last_layout_.sidebar.width - 0.5F, height, hairline);
  const float side_x = last_layout_.sidebar.x;
  const bool compact = last_layout_.compact_sidebar;
  if (!compact) {
    label(surface, "工作空间", {side_x + 16, 60, 90, 18}, 10, muted, 650);
    label(surface, workspace_.filename().string(),
          {side_x + 16, 79, 166, 23}, 13, ink, 600);
    const Rect sidebar_search{side_x + 182, 65, 30, 30};
    if (hovered(sidebar_search))
      surface.fill_rect(sidebar_search, hover_fill, 7);
    draw_icon(surface, "search", side_x + 197, 80, secondary);
    add_hit(sidebar_search, WorkbenchActionKind::focus_filter);
  } else {
    surface.fill_circle(side_x + 36, 72, 15, ink);
    label(surface, "T", {side_x + 24, 60, 24, 24}, 13,
          {255, 255, 255, 255}, 700, 1, white::TextAlign::center);
  }

  const Rect create{side_x + 12, 108, last_layout_.sidebar.width - 24, 38};
  surface.fill_rect(create,
                    hovered(create) ? Color{49, 51, 56, 255} : ink, 10);
  draw_icon(surface, "plus", compact ? side_x + 36 : side_x + 29,
            create.y + 19, {255, 255, 255, 255});
  if (!compact)
    label(surface, "新建会话", {side_x + 48, create.y + 9, 120, 20}, 12,
          {255, 255, 255, 255}, 600);
  add_hit(create, WorkbenchActionKind::new_session);

  struct NavItem {
    std::string_view icon;
    std::string_view label;
    WorkbenchActionKind action;
  };
  constexpr NavItem nav[] = {{"chat", "会话", WorkbenchActionKind::redraw},
                             {"plugin", "插件", WorkbenchActionKind::inspect_composition},
                             {"pulse", "诊断", WorkbenchActionKind::diagnostics}};
  float nav_y = 156;
  for (std::size_t index = 0; index < std::size(nav); ++index) {
    const auto& item = nav[index];
    const Rect row{side_x + 8, nav_y, last_layout_.sidebar.width - 16, 34};
    if (index == 0 || hovered(row))
      surface.fill_rect(row,
                        hovered(row) ? hover_fill : Color{235, 235, 232, 255},
                        8);
    draw_icon(surface, item.icon, compact ? side_x + 36 : side_x + 24,
              nav_y + 17, secondary);
    if (!compact)
      label(surface, item.label, {side_x + 44, nav_y + 8, 146, 20}, 12,
            index == 0 ? ink : secondary, index == 0 ? 600 : 500);
    add_hit(row, item.action);
    nav_y += 37;
  }

  if (!compact) {
    label(surface, "会话", {side_x + 16, 279, 80, 18}, 10, muted, 650);
    const Rect session_area{side_x + 8, 301,
                            last_layout_.sidebar.width - 16,
                            std::max(40.0F, height - 443.0F)};
    session_max_scroll_ = std::max(
        0.0F, static_cast<float>(frame.sessions.size()) * 36.0F -
                  session_area.height);
    session_scroll_ =
        std::clamp(session_scroll_, 0.0F, session_max_scroll_);
    surface.push_clip(session_area);
    float session_y = session_area.y - session_scroll_;
    if (frame.sessions.empty())
      label(surface, "暂无会话", {session_area.x + 8, session_y + 8,
                                  session_area.width - 16, 20},
            11, muted, 450);
    for (const auto& session : frame.sessions) {
      const Rect row{session_area.x, session_y, session_area.width, 32};
      const bool selected = session.id == frame.session_id;
      if (selected || hovered(row))
        surface.fill_rect(row,
                          selected ? selected_fill : hover_fill, 8);
      draw_icon(surface, "chat", row.x + 16, row.y + 16,
                selected ? ink : muted);
      label(surface, session.title,
            {row.x + 35, row.y + 6, row.width - 43, 20}, 11,
            session.closed ? muted : (selected ? ink : secondary),
            selected ? 600 : 450);
      if (row.y + row.height > session_area.y &&
          row.y < session_area.y + session_area.height)
        hits_.push_back({row, WorkbenchActionKind::switch_session, {},
                         session.id});
      session_y += 36;
    }
    surface.pop_clip();
    if (session_max_scroll_ > 0) {
      const float ratio = session_area.height /
                          (session_area.height + session_max_scroll_);
      const float thumb = std::max(24.0F, session_area.height * ratio);
      const float progress = session_scroll_ / session_max_scroll_;
      surface.fill_rect({session_area.x + session_area.width - 3,
                         session_area.y +
                             (session_area.height - thumb) * progress,
                         2, thumb},
                        {168, 169, 166, 145}, 2);
    }

    const Rect runtime{side_x + 12, height - 126,
                       last_layout_.sidebar.width - 24, 46};
    surface.fill_rect(runtime, {247, 247, 245, 255}, 10);
    surface.fill_circle(runtime.x + 15, runtime.y + 16, 3.5F,
                        frame.snow_connected ? success : danger);
    label(surface, frame.snow_connected ? "Arche 系统就绪" : "Snow 未连接",
          {runtime.x + 27, runtime.y + 7, runtime.width - 35, 19}, 11, ink,
          600);
    label(surface, "4 个运行时服务", {runtime.x + 27, runtime.y + 24,
                                      runtime.width - 35, 17},
          9, muted, 500);

    const float footer_y = height - 66;
    surface.fill_circle(side_x + 25, footer_y + 16, 13,
                        {42, 103, 145, 255});
    label(surface, "T", {side_x + 15, footer_y + 7, 20, 20}, 10,
          {255, 255, 255, 255}, 700, 1, white::TextAlign::center);
    label(surface, "Tokmon User", {side_x + 46, footer_y + 6, 120, 20},
          11, ink, 600);
    label(surface,
          "epoch " + std::to_string(frame.composition_epoch) + " · seq " +
              std::to_string(frame.trajectory_cursor),
          {side_x + 46, footer_y + 24, 150, 18}, 9, muted, 500);
  } else {
    session_scroll_ = 0;
    session_max_scroll_ = 0;
    surface.fill_circle(side_x + 36, height - 42, 12,
                        frame.snow_connected ? success : danger);
  }
  }

  // Conversation panel and header.
  const auto& conversation = last_layout_.conversation;
  surface.fill_rect(conversation, panel);
  surface.line(conversation.x + conversation.width - 0.5F, conversation.y,
               conversation.x + conversation.width - 0.5F,
               conversation.y + conversation.height, hairline);
  surface.line(conversation.x, conversation.y + 58, conversation.x +
                   conversation.width,
               conversation.y + 58, hairline);
  surface.fill_circle(conversation.x + 25, conversation.y + 29, 3.5F,
                      frame.turn_active ? accent : success);
  label(surface, normalized_title(frame),
        {conversation.x + 38, conversation.y + 17,
         conversation.width - 210, 25},
        13, ink, 650);
  const Rect fork_button{conversation.x + conversation.width - 104,
                         conversation.y + 14, 32, 30};
  if (hovered(fork_button)) surface.fill_rect(fork_button, hover_fill, 7);
  draw_icon(surface, "fork", fork_button.x + 16, fork_button.y + 15,
            secondary);
  add_hit(fork_button, WorkbenchActionKind::fork_session);
  const Rect inspect_button{conversation.x + conversation.width - 64,
                            conversation.y + 14, 32, 30};
  if (hovered(inspect_button))
    surface.fill_rect(inspect_button, hover_fill, 7);
  label(surface, "•••", {inspect_button.x, inspect_button.y + 5,
                          inspect_button.width, 20},
        11, secondary, 600, 1, white::TextAlign::center);
  add_hit(inspect_button, WorkbenchActionKind::inspect_composition);

  // Conversation timeline.
  const auto& timeline = last_layout_.timeline;
  float content_height = 30;
  for (const auto& item : frame.items)
    content_height += item_height(item, timeline.width - 64) + 14;
  timeline_max_scroll_ = std::max(0.0F, content_height - timeline.height);
  if (frame.items.size() != previous_item_count_) {
    if (follow_tail_) timeline_scroll_ = timeline_max_scroll_;
    previous_item_count_ = frame.items.size();
  }
  timeline_scroll_ = std::clamp(timeline_scroll_, 0.0F, timeline_max_scroll_);
  surface.push_clip(timeline);
  float item_y = timeline.y + 20 - timeline_scroll_;
  if (frame.items.empty()) {
    const float welcome_width = std::min(390.0F, timeline.width - 70);
    const Rect welcome{timeline.x + (timeline.width - welcome_width) / 2,
                       timeline.y + 72, welcome_width, 210};
    surface.fill_circle(welcome.x + welcome.width / 2, welcome.y + 24, 20,
                        {42, 44, 48, 255});
    label(surface, "A", {welcome.x + welcome.width / 2 - 12,
                          welcome.y + 12, 24, 24},
          14, {255, 255, 255, 255}, 700, 1, white::TextAlign::center);
    label(surface, "Arche Agent OS 已就绪",
          {welcome.x, welcome.y + 56, welcome.width, 30}, 18, ink, 650, 1,
          white::TextAlign::center);
    label(surface,
          "描述你的目标。Snow 会记录完整轨迹，按策略调用能力，并把结果投影到这里。",
          {welcome.x + 20, welcome.y + 92, welcome.width - 40, 52}, 13,
          secondary, 400, 3, white::TextAlign::center);
    const Rect suggestion{welcome.x + 34, welcome.y + 160,
                          welcome.width - 68, 34};
    surface.fill_rect(suggestion,
                      hovered(suggestion) ? hover_fill
                                          : Color{248, 248, 247, 255},
                      8);
    surface.stroke_rect(suggestion,
                        hovered(suggestion) ? hover_border : hairline, 1, 8);
    label(surface, "检查当前工作区并给出下一步建议", suggestion, 12,
          secondary, 450, 1, white::TextAlign::center);
    hits_.push_back(
        {suggestion, WorkbenchActionKind::set_message_input, {},
         "检查当前工作区并给出下一步建议"});
  } else {
    for (const auto& item : frame.items) {
      const float card_height = item_height(item, timeline.width - 64);
      if (item_y + card_height > timeline.y &&
          item_y < timeline.y + timeline.height) {
        if (item.kind == ItemKind::user) {
          const auto rows = estimated_rows(
              item.content, std::min(timeline.width * 0.76F, 450.0F), 14);
          const float bubble_width = std::clamp(
              38.0F + visual_units(item.content) * 8.1F,
              170.0F, std::min(timeline.width * 0.76F, 450.0F));
          const float bubble_height = 22.0F + static_cast<float>(rows) * 21.0F;
          const Rect bubble{timeline.x + timeline.width - bubble_width - 24,
                            item_y, bubble_width, bubble_height};
          surface.fill_rect(bubble, {239, 240, 238, 255}, 15);
          label(surface, item.content,
                {bubble.x + 14, bubble.y + 10, bubble.width - 28,
                 bubble.height - 17},
                14, ink, 400, rows, white::TextAlign::left, false, 1.5F);
          const float meta_y = bubble.y + bubble.height + 7;
          const Rect edit{bubble.x + bubble.width - 20, meta_y, 20, 20};
          const Rect copy{edit.x - 27, meta_y, 20, 20};
          label(surface, clock_label(item.metadata),
                {bubble.x + bubble.width - 104, meta_y + 2, 50, 18}, 10,
                muted, 400, 1, white::TextAlign::right);
          if (hovered(copy)) surface.fill_rect(copy, hover_fill, 6);
          if (hovered(edit)) surface.fill_rect(edit, hover_fill, 6);
          draw_icon(surface, "copy", copy.x + 10, copy.y + 9,
                    hovered(copy) ? accent : muted);
          draw_icon(surface, "edit", edit.x + 10, edit.y + 9,
                    hovered(edit) ? accent : muted);
          hits_.push_back(
              {copy, WorkbenchActionKind::copy_text, {}, item.content});
          hits_.push_back({edit, WorkbenchActionKind::set_message_input, {},
                           item.content});
        } else if (item.kind == ItemKind::assistant) {
          const float flow_x = timeline.x + 26;
          const float flow_width = timeline.width - 52;
          label(surface, elapsed_label(item) + "  ›",
                {flow_x, item_y + 2, 180, 20}, 11, muted, 450);
          if (item.status == "streaming")
            surface.fill_circle(flow_x - 10, item_y + 11, 3, accent);
          surface.line(flow_x, item_y + 29, flow_x + flow_width,
                       item_y + 29, hairline, 1);
          draw_markdown(surface, item.content, flow_x, item_y + 42,
                        flow_width);
        } else {
          const float card_width = timeline.width - 52;
          const Rect card{timeline.x + 26, item_y, card_width, card_height};
          Color card_fill{250, 250, 249, 255};
          Color border = hairline;
          if (item.kind == ItemKind::artifact) {
            card_fill = {240, 250, 244, 255};
            border = {196, 228, 207, 255};
          } else if (item.kind == ItemKind::diagnostic) {
            card_fill = {246, 244, 253, 255};
            border = {220, 214, 242, 255};
          } else if (item.kind == ItemKind::error) {
            card_fill = {255, 242, 242, 255};
            border = {242, 198, 198, 255};
          }
          surface.fill_rect(card, card_fill, 9);
          surface.stroke_rect(card, border, 1, 9);
          surface.fill_circle(card.x + 12, card.y + 14, 3,
                              item.kind == ItemKind::error ? danger : success);
          label(surface, item.title,
                {card.x + 22, card.y + 6, card.width - 120, 20}, 11,
                item.kind == ItemKind::error ? danger : secondary, 600);
          label(surface, item.status,
                {card.x + card.width - 90, card.y + 6, 76, 20}, 10, muted,
                450, 1, white::TextAlign::right);
          label(surface, item.content,
                {card.x + 14, card.y + 31, card.width - 28,
                 card.height - 38},
                item.kind == ItemKind::tool ? 12.0F : 13.0F,
                item.kind == ItemKind::error ? danger : ink, 400, 5,
                white::TextAlign::left, item.kind == ItemKind::tool, 1.42F);
        }
      }
      item_y += card_height + 14;
    }
  }
  surface.pop_clip();
  if (timeline_max_scroll_ > 0) {
    const float ratio = timeline.height / (timeline.height + timeline_max_scroll_);
    const float thumb = std::max(28.0F, timeline.height * ratio);
    const float progress = timeline_scroll_ / timeline_max_scroll_;
    surface.fill_rect({timeline.x + timeline.width - 5, timeline.y + 3 +
                           (timeline.height - thumb - 6) * progress,
                       3, thumb},
                      {170, 172, 176, 150}, 2);
    const Rect tail{timeline.x + timeline.width / 2 - 17,
                    last_layout_.composer.y - 44, 34, 34};
    surface.fill_circle(tail.x + 17, tail.y + 18, 17,
                        hovered(tail) ? Color{238, 243, 251, 255}
                                      : Color{255, 255, 255, 245});
    surface.stroke_rect(tail, hovered(tail) ? Color{174, 197, 230, 255}
                                            : hairline,
                        1, 17);
    draw_icon(surface, "down", tail.x + 17, tail.y + 17,
              hovered(tail) ? accent : secondary);
    add_hit(tail, WorkbenchActionKind::scroll_to_tail);
  }

  // Composer. White owns the actual editor; this is its product projection.
  const auto& composer = last_layout_.composer;
  surface.fill_rect({composer.x + 1, composer.y + 5, composer.width,
                     composer.height},
                    {70, 72, 78, 28}, 15);
  surface.fill_rect(composer, {255, 255, 254, 255}, 15);
  surface.stroke_rect(composer,
                      frame.message_focused ? Color{199, 208, 230, 255}
                                            : Color{215, 215, 212, 255},
                      frame.message_focused ? 1.2F : 1.0F, 15);
  const Rect message_editor{composer.x + 16, composer.y + 12,
                            composer.width - 32, 40};
  message_editor_bounds_ = message_editor;
  message_editor_text_ = frame.message_input;
  if (frame.message_input.empty()) {
    label(surface, "输入消息，Enter 发送，Shift+Enter 换行", message_editor,
          13, muted, 400, 2);
    if (frame.message_focused && frame.caret_visible)
      surface.line(message_editor.x, message_editor.y + 1,
                   message_editor.x, message_editor.y + 18,
                   {38, 92, 190, 255}, 1.4F);
  } else {
    draw_editor_text(surface, frame.message_input, message_editor,
                     frame.editor_cursor, frame.selection_start,
                     frame.selection_end, frame.message_focused,
                     frame.caret_visible);
  }
  const Rect attach{composer.x + 12, composer.y + 55, 26, 24};
  if (hovered(attach)) surface.fill_rect(attach, hover_fill, 7);
  draw_icon(surface, "plus", attach.x + 13, attach.y + 12, secondary);
  add_hit(attach, WorkbenchActionKind::attach_files);
  const Rect access{composer.x + 47, composer.y + 56, 72, 22};
  surface.fill_circle(access.x + 6, access.y + 11, 3, warning);
  label(surface, "完全访问", {access.x + 14, access.y + 3, 50, 18}, 9,
        secondary, 600);
  const auto model = frame.model.empty() ? "默认模型" : frame.model;
  label(surface, model, {composer.x + composer.width - 170, composer.y + 61,
                         116, 18},
        9, muted, 550, 1, white::TextAlign::right);
  const Rect send{composer.x + composer.width - 42, composer.y + 52, 32, 32};
  surface.fill_circle(send.x + 16, send.y + 16, 15,
                      frame.turn_active
                          ? (hovered(send) ? Color{242, 211, 211, 255}
                                           : Color{249, 232, 232, 255})
                          : (hovered(send) ? accent_hover
                                           : Color{65, 67, 72, 255}));
  draw_icon(surface, frame.turn_active ? "stop" : "send", send.x + 16,
            send.y + 16,
            frame.turn_active ? danger : Color{255, 255, 255, 255});
  add_hit(send, frame.turn_active ? WorkbenchActionKind::cancel_turn
                                  : WorkbenchActionKind::submit_input);
  if (!frame.attachments.empty()) {
    float attachment_x = composer.x + 8;
    const float attachment_y = composer.y - 29;
    for (std::size_t index = 0;
         index < frame.attachments.size() && index < 4; ++index) {
      const auto& attachment = frame.attachments[index];
      const float chip_width = std::clamp(
          56.0F + static_cast<float>(utf8_length(attachment.name)) * 5.5F,
          92.0F, 170.0F);
      if (attachment_x + chip_width > composer.x + composer.width) break;
      const Rect chip{attachment_x, attachment_y, chip_width, 23};
      surface.fill_rect(chip, hovered(chip) ? Color{235, 240, 249, 255}
                                           : Color{244, 247, 252, 255},
                        7);
      surface.stroke_rect(chip,
                          hovered(chip) ? Color{172, 195, 229, 255}
                                        : Color{207, 217, 233, 255},
                          1, 7);
      draw_icon(surface, "file", chip.x + 12, chip.y + 11, accent);
      label(surface, attachment.name,
            {chip.x + 25, chip.y + 4, chip.width - 43, 17}, 10, secondary,
            500);
      const Rect remove{chip.x + chip.width - 21, chip.y + 2, 18, 19};
      if (hovered(chip))
        surface.fill_rect(remove, Color{222, 230, 243, 255}, 6);
      label(surface, "×", remove, 11, hovered(chip) ? danger : muted, 550, 1,
            white::TextAlign::center);
      hits_.push_back({chip, WorkbenchActionKind::remove_attachment, {}, {},
                       index});
      attachment_x += chip_width + 6;
    }
  }

  // Approval card overlays the timeline while preserving the conversation.
  if (frame.approval) {
    const float modal_width = std::min(420.0F, conversation.width - 60);
    const Rect modal{conversation.x + (conversation.width - modal_width) / 2,
                     conversation.y + 100, modal_width, 238};
    surface.fill_rect({modal.x + 3, modal.y + 5, modal.width, modal.height},
                      {50, 50, 55, 35}, 13);
    surface.fill_rect(modal, {255, 252, 245, 255}, 13);
    surface.stroke_rect(modal, {235, 203, 137, 255}, 1, 13);
    surface.fill_circle(modal.x + 25, modal.y + 26, 10,
                        {255, 235, 194, 255});
    label(surface, "!", {modal.x + 18, modal.y + 17, 14, 20}, 12, warning,
          700, 1, white::TextAlign::center);
    label(surface, "需要批准 · " + frame.approval->tool.name,
          {modal.x + 44, modal.y + 15, modal.width - 60, 24}, 14, ink, 650);
    label(surface, frame.approval->reason,
          {modal.x + 18, modal.y + 51, modal.width - 36, 44}, 12,
          secondary, 400, 2);
    surface.fill_rect({modal.x + 18, modal.y + 101, modal.width - 36, 68},
                      {249, 247, 241, 255}, 8);
    label(surface, frame.approval->arguments.dump(2),
          {modal.x + 28, modal.y + 110, modal.width - 56, 51}, 10,
          secondary, 400, 3, white::TextAlign::left, true);
    const Rect deny{modal.x + modal.width - 180, modal.y + 188, 74, 32};
    const Rect approve{modal.x + modal.width - 96, modal.y + 188, 78, 32};
    button(deny, "拒绝", WorkbenchActionKind::deny);
    button(approve, "批准", WorkbenchActionKind::approve, true);
  }

  // File/document viewer, matching the reference's docked inspector.
  if (last_layout_.viewer_visible) {
    const auto& viewer = last_layout_.viewer;
    surface.fill_rect(viewer, panel);
    surface.line(viewer.x, viewer.y, viewer.x,
                 viewer.y + viewer.height, hairline);
    surface.line(viewer.x, viewer.y + 48, viewer.x + viewer.width,
                 viewer.y + 48, hairline);
    float tab_x = viewer.x + 14;
    const float available_tabs = viewer.width - 68;
    const auto visible_tabs =
        std::max<std::size_t>(1, std::min<std::size_t>(3, open_documents_.size()));
    const float tab_width = std::clamp(
        available_tabs / static_cast<float>(visible_tabs), 112.0F, 180.0F);
    for (std::size_t index = 0;
         index < open_documents_.size() && index < 3; ++index) {
      const auto& document = open_documents_[index];
      const Rect tab{tab_x, viewer.y + 9, tab_width, 32};
      const auto selected = document == selected_document_;
      if (selected || hovered(tab)) {
        const auto tab_fill = selected
                                  ? (hovered(tab)
                                         ? Color{229, 229, 227, 255}
                                         : Color{244, 244, 243, 255})
                                  : hover_fill;
        surface.fill_rect(tab, tab_fill, 8);
      }
      draw_icon(surface, "file", tab.x + 17, tab.y + 15,
                selected ? secondary : muted);
      label(surface, document.filename().string(),
            {tab.x + 32, tab.y + 7, tab.width - 54, 20}, 11,
            selected ? ink : secondary, selected ? 550 : 400);
      const Rect close{tab.x + tab.width - 21, tab.y + 6, 17, 18};
      if (hovered(close))
        surface.fill_rect(close, Color{224, 224, 222, 255}, 6);
      label(surface, "×", close, 11, hovered(close) ? danger : muted, 500, 1,
            white::TextAlign::center);
      hits_.push_back({tab, WorkbenchActionKind::redraw, document});
      hits_.push_back({close, WorkbenchActionKind::redraw, document, {}, 0,
                       false, true});
      tab_x += tab_width + 4;
    }
    const Rect add_tab{tab_x + 3, viewer.y + 10, 28, 30};
    if (hovered(add_tab)) surface.fill_rect(add_tab, hover_fill, 7);
    label(surface, "+", {add_tab.x, add_tab.y + 5, add_tab.width, 20}, 16,
          muted, 400, 1, white::TextAlign::center);
    add_hit(add_tab, WorkbenchActionKind::open_file_dialog);
    surface.line(viewer.x, viewer.y + 92, viewer.x + viewer.width,
                 viewer.y + 92, hairline);
    label(surface, workspace_.filename().string() + "  ›  " +
                       selected_document_.generic_string(),
          {viewer.x + 18, viewer.y + 62, viewer.width - 170, 20}, 10,
          secondary, 450);
    const Rect inspect{viewer.x + viewer.width - 140, viewer.y + 57, 122, 27};
    if (hovered(inspect)) surface.fill_rect(inspect, hover_fill, 7);
    label(surface, "查看 Arche 状态",
          {inspect.x + 5, inspect.y + 5, inspect.width - 10, 18}, 11, ink,
          550, 1, white::TextAlign::center);
    add_hit(inspect, WorkbenchActionKind::inspect_composition);

    const auto& doc = last_layout_.document;
    surface.push_clip(doc);
    float doc_y = doc.y + 24 - document_scroll_;
    bool code = false;
    for (const auto& original : document_lines_) {
      auto line = original;
      if (line.starts_with("```")) {
        code = !code;
        doc_y += 10;
        continue;
      }
      float line_size = 13;
      float line_extent = 23;
      int weight = 400;
      Color line_color = ink;
      if (line.starts_with("### ")) {
        line.erase(0, 4);
        line_size = 15;
        line_extent = 31;
        weight = 650;
      } else if (line.starts_with("## ")) {
        line.erase(0, 3);
        line_size = 18;
        line_extent = 39;
        weight = 650;
      } else if (line.starts_with("# ")) {
        line.erase(0, 2);
        line_size = 23;
        line_extent = 50;
        weight = 700;
      } else if (line.empty()) {
        line_extent = 13;
      } else {
        const auto average_glyph = code ? 7.6F : 10.5F;
        const auto columns = std::max<std::size_t>(
            24, static_cast<std::size_t>((doc.width - 44) / average_glyph));
        const auto wrapped = std::clamp<std::size_t>(
            (utf8_length(line) + columns - 1) / columns, 1, 4);
        line_extent = static_cast<float>(wrapped) * (code ? 20.0F : 22.0F);
      }
      if (!code) line = markdown_inline(std::move(line));
      if (code && !line.empty()) {
        surface.fill_rect({doc.x + 16, doc_y - 4, doc.width - 32,
                           line_extent + 7},
                          {248, 248, 247, 255}, 4);
        line_color = {64, 67, 73, 255};
      }
      if (!line.empty()) {
        const auto measured = label(
            surface, line, {doc.x + 22, doc_y, doc.width - 44, 240},
            line_size, line_color, weight, 4, white::TextAlign::left, code,
            code ? 1.45F : 1.48F);
        line_extent = std::max(line_extent, measured + 2);
      }
      doc_y += line_extent;
    }
    document_max_scroll_ =
        std::max(0.0F, doc_y + document_scroll_ - doc.y - doc.height + 20);
    document_scroll_ =
        std::clamp(document_scroll_, 0.0F, document_max_scroll_);
    surface.pop_clip();
    if (document_max_scroll_ > 0) {
      const float ratio = doc.height / (doc.height + document_max_scroll_);
      const float thumb = std::max(30.0F, doc.height * ratio);
      const float progress = document_scroll_ / document_max_scroll_;
      surface.fill_rect({doc.x + doc.width - 5, doc.y + 3 +
                             (doc.height - thumb - 6) * progress,
                         3, thumb},
                        {150, 152, 158, 180}, 2);
    }

    const auto& explorer = last_layout_.explorer;
    surface.fill_rect(explorer, {250, 250, 249, 255});
    surface.line(explorer.x, explorer.y, explorer.x,
                 explorer.y + explorer.height, hairline);
    const Rect search{explorer.x + 10, explorer.y + 10,
                      explorer.width - 20, 30};
    surface.fill_rect(search, hovered(search) ? Color{252, 252, 251, 255}
                                              : panel,
                      7);
    surface.stroke_rect(search,
                        frame.filter_focused
                            ? accent
                            : (hovered(search) ? hover_border : hairline),
                        frame.filter_focused ? 1.4F : 1.0F, 7);
    draw_icon(surface, "search", search.x + 15, search.y + 15, muted);
    const Rect filter_editor{search.x + 30, search.y + 6,
                             search.width - 38, 20};
    filter_editor_bounds_ = filter_editor;
    filter_editor_text_ = frame.file_filter;
    if (frame.file_filter.empty()) {
      label(surface, "筛选文件…", filter_editor, 11, muted);
      if (frame.filter_focused && frame.caret_visible)
        surface.line(filter_editor.x, filter_editor.y + 1,
                     filter_editor.x, filter_editor.y + 16, accent, 1.3F);
    } else {
      draw_editor_text(surface, frame.file_filter, filter_editor,
                       frame.editor_cursor, frame.selection_start,
                       frame.selection_end, frame.filter_focused,
                       frame.caret_visible, 11, 1);
    }
    add_hit(search, WorkbenchActionKind::focus_filter);
    float file_y = explorer.y + 51;
    draw_icon(surface, "chevron", explorer.x + 16, file_y + 10, secondary);
    draw_icon(surface, "folder", explorer.x + 34, file_y + 10, secondary);
    label(surface, workspace_.filename().string(),
          {explorer.x + 50, file_y, explorer.width - 58, 22}, 12, ink, 600);
    file_y += 27;
    for (const auto& entry : files_) {
      if (file_y + 25 > explorer.y + explorer.height) break;
      const Rect row{explorer.x + 5, file_y, explorer.width - 10, 25};
      const bool selected = !selected_document_.empty() &&
                            entry.relative == selected_document_;
      if (selected || hovered(row))
        surface.fill_rect(row, selected ? selected_fill : hover_fill, 6);
      const float indent = static_cast<float>(entry.depth) * 13.0F;
      if (entry.directory) {
        if (expanded_directories_.contains(entry.relative) ||
            !frame.file_filter.empty())
          label(surface, "⌄", {row.x + 8 + indent, row.y + 3, 18, 18}, 11,
                muted, 500, 1, white::TextAlign::center);
        else
          draw_icon(surface, "chevron", row.x + 17 + indent,
                    row.y + 12, muted);
      }
      draw_icon(surface, entry.directory ? "folder" : "file",
                row.x + 34 + indent,
                row.y + 12, entry.directory ? secondary : muted);
      label(surface, entry.label,
            {row.x + 49 + indent, row.y + 3,
             row.width - 55 - indent, 20}, 11,
            selected ? ink : secondary, selected ? 600 : 400);
      hits_.push_back({row, WorkbenchActionKind::redraw, entry.relative, {},
                       0, entry.directory});
      file_y += 26;
    }
  }

  // Resize affordances sit above pane content. A six-pixel hit strip keeps
  // dragging easy while the visible divider remains a quiet one-pixel line.
  if (last_layout_.sidebar_splitter.width > 0) {
    const auto& splitter = last_layout_.sidebar_splitter;
    if (hovered(splitter) || resizing_sidebar_)
      surface.fill_rect({splitter.x + 2, splitter.y, 2, splitter.height},
                        {82, 115, 211, 190});
  }
  if (last_layout_.viewer_splitter.width > 0) {
    const auto& splitter = last_layout_.viewer_splitter;
    if (hovered(splitter) || resizing_viewer_)
      surface.fill_rect({splitter.x + 2, splitter.y, 2, splitter.height},
                        {82, 115, 211, 190});
  }

  // Native-style application menus are projected by White so every entry is
  // testable and wired to the same action system as the rest of the workbench.
  if (!active_menu_.empty()) {
    struct MenuEntry {
      std::string label;
      WorkbenchActionKind action;
      std::string value;
      std::string hint;
    };
    std::vector<MenuEntry> entries;
    float menu_x = 102;
    if (active_menu_ == "file") {
      entries = {{"新建会话", WorkbenchActionKind::new_session, {}, {}},
                 {"打开工作区文件", WorkbenchActionKind::open_file_dialog,
                  {}, {}},
                 {"添加附件", WorkbenchActionKind::attach_files, {}, {}}};
    } else if (active_menu_ == "edit") {
      menu_x = 152;
      entries = {{"聚焦输入框", WorkbenchActionKind::focus_message, {}, {}},
                 {"清空输入", WorkbenchActionKind::set_message_input, {}, {}}};
      for (const auto& item : std::views::reverse(frame.items)) {
        if (item.kind == ItemKind::user) {
          entries.push_back({"编辑上一条消息",
                             WorkbenchActionKind::set_message_input,
                             item.content, {}});
          break;
        }
      }
      for (const auto& item : std::views::reverse(frame.items)) {
        if (item.kind == ItemKind::assistant) {
          entries.push_back({"复制上一条回复", WorkbenchActionKind::copy_text,
                             item.content, {}});
          break;
        }
      }
    } else if (active_menu_ == "view") {
      menu_x = 202;
      entries = {{sidebar_collapsed_ ? "展开会话栏" : "折叠会话栏",
                  WorkbenchActionKind::toggle_left_panel, {}, {}},
                 {viewer_collapsed_ ? "展开工作区" : "折叠工作区",
                  WorkbenchActionKind::toggle_right_panel, {}, {}},
                 {"查看 Arche 状态",
                  WorkbenchActionKind::inspect_composition, {}, {}}};
    } else if (active_menu_ == "help") {
      menu_x = 252;
      entries = {{"命令与快捷方式", WorkbenchActionKind::show_help, {}, {}},
                 {"运行诊断", WorkbenchActionKind::diagnostics, {}, {}},
                 {"查看组合状态", WorkbenchActionKind::inspect_composition,
                  {}, {}}};
    }
    if (!entries.empty()) {
      const float menu_width = 210;
      const Rect menu{menu_x, 42, menu_width,
                      12.0F + static_cast<float>(entries.size()) * 34.0F};
      open_menu_bounds_ = menu;
      surface.fill_rect({menu.x + 3, menu.y + 5, menu.width, menu.height},
                        {42, 43, 48, 28}, 10);
      surface.fill_rect(menu, {255, 255, 254, 255}, 10);
      surface.stroke_rect(menu, {214, 214, 211, 255}, 1, 10);
      float row_y = menu.y + 6;
      for (const auto& entry : entries) {
        const Rect row{menu.x + 6, row_y, menu.width - 12, 30};
        if (hovered(row)) surface.fill_rect(row, hover_fill, 7);
        label(surface, entry.label,
              {row.x + 10, row.y + 6, row.width - 70, 19}, 11, ink, 500);
        if (!entry.hint.empty())
          label(surface, entry.hint,
                {row.x + row.width - 62, row.y + 6, 52, 19}, 9, muted, 450,
                1, white::TextAlign::right);
        hits_.push_back({row, entry.action, {}, entry.value});
        row_y += 34;
      }
    }
  }
}

WorkbenchAction WorkbenchView::dispatch(const white::UiEvent& event) {
  if (event.type == "pointerdown") {
    if (last_layout_.sidebar_splitter.contains(event.x, event.y)) {
      resizing_sidebar_ = true;
      resizing_viewer_ = false;
      selecting_input_ = false;
      return {WorkbenchActionKind::redraw, {}, 0, 0, false, true};
    }
    if (last_layout_.viewer_splitter.contains(event.x, event.y)) {
      resizing_viewer_ = true;
      resizing_sidebar_ = false;
      selecting_input_ = false;
      return {WorkbenchActionKind::redraw, {}, 0, 0, false, true};
    }
    if (message_editor_bounds_.contains(event.x, event.y)) {
      selecting_input_ = true;
      selecting_filter_ = false;
      return {WorkbenchActionKind::focus_message, {}, 0,
              editor_offset_at(event.x, event.y, message_editor_bounds_,
                               message_editor_text_),
              false};
    }
    if (filter_editor_bounds_.contains(event.x, event.y)) {
      selecting_input_ = true;
      selecting_filter_ = true;
      return {WorkbenchActionKind::focus_filter, {}, 0,
              editor_offset_at(event.x, event.y, filter_editor_bounds_,
                               filter_editor_text_),
              false};
    }
    selecting_input_ = false;
    return {};
  }
  if (event.type == "pointermove") {
    pointer_x_ = event.x;
    pointer_y_ = event.y;
    if (resizing_sidebar_) {
      const auto content_reserve =
          last_layout_.menu_bar.width >= 1160 && !viewer_collapsed_
              ? 820.0F
              : 560.0F;
      const auto max_sidebar = std::max(
          176.0F, last_layout_.menu_bar.width - content_reserve);
      sidebar_width_ = std::clamp(event.x, 176.0F, max_sidebar);
      sidebar_collapsed_ = false;
      sidebar_manually_sized_ = true;
      return {WorkbenchActionKind::redraw, {}, 0, 0, false, true};
    }
    if (resizing_viewer_) {
      const auto available = last_layout_.menu_bar.width -
                             last_layout_.sidebar.width;
      const auto max_viewer = std::max(320.0F, available - 500.0F);
      viewer_width_ = std::clamp(last_layout_.menu_bar.width - event.x,
                                 320.0F, max_viewer);
      viewer_collapsed_ = false;
      viewer_manually_sized_ = true;
      return {WorkbenchActionKind::redraw, {}, 0, 0, false, true};
    }
    if (selecting_input_) {
      const auto& bounds = selecting_filter_ ? filter_editor_bounds_
                                             : message_editor_bounds_;
      const auto& text = selecting_filter_ ? filter_editor_text_
                                           : message_editor_text_;
      return {WorkbenchActionKind::set_editor_cursor, {}, 0,
              editor_offset_at(event.x, event.y, bounds, text), true, false};
    }
    const bool over_splitter =
        last_layout_.sidebar_splitter.contains(event.x, event.y) ||
        last_layout_.viewer_splitter.contains(event.x, event.y);
    return {WorkbenchActionKind::redraw, {}, 0, 0, false, over_splitter};
  }
  if (event.type == "wheel") {
    if (last_layout_.sidebar.contains(event.x, event.y) &&
        event.y >= 301) {
      session_scroll_ = std::clamp(session_scroll_ + event.delta_y, 0.0F,
                                   session_max_scroll_);
      return {WorkbenchActionKind::redraw};
    }
    if (last_layout_.timeline.contains(event.x, event.y)) {
      timeline_scroll_ = std::clamp(timeline_scroll_ + event.delta_y, 0.0F,
                                    timeline_max_scroll_);
      follow_tail_ = timeline_scroll_ >= timeline_max_scroll_ - 2;
      return {WorkbenchActionKind::redraw};
    }
    if (last_layout_.document.contains(event.x, event.y)) {
      document_scroll_ = std::clamp(document_scroll_ + event.delta_y, 0.0F,
                                    document_max_scroll_);
      return {WorkbenchActionKind::redraw};
    }
    return {};
  }
  if (event.type != "click") return {};
  if (resizing_sidebar_ || resizing_viewer_) {
    resizing_sidebar_ = false;
    resizing_viewer_ = false;
    const bool over_splitter =
        last_layout_.sidebar_splitter.contains(event.x, event.y) ||
        last_layout_.viewer_splitter.contains(event.x, event.y);
    return {WorkbenchActionKind::redraw, {}, 0, 0, false, over_splitter};
  }
  if (selecting_input_) {
    selecting_input_ = false;
    return {WorkbenchActionKind::redraw};
  }
  const Rect menu_headers{102, 7, 196, 30};
  if (!active_menu_.empty() &&
      !open_menu_bounds_.contains(event.x, event.y) &&
      !menu_headers.contains(event.x, event.y)) {
    active_menu_.clear();
    return {WorkbenchActionKind::redraw};
  }
  for (const auto& target : std::views::reverse(hits_)) {
    if (!target.bounds.contains(event.x, event.y)) continue;
    if (target.action == WorkbenchActionKind::toggle_menu) {
      active_menu_ = active_menu_ == target.value ? "" : target.value;
      return {WorkbenchActionKind::redraw};
    }
    if (target.action == WorkbenchActionKind::toggle_left_panel) {
      sidebar_collapsed_ = !sidebar_collapsed_;
      active_menu_.clear();
      return {WorkbenchActionKind::redraw};
    }
    if (target.action == WorkbenchActionKind::toggle_right_panel) {
      viewer_collapsed_ = !viewer_collapsed_;
      active_menu_.clear();
      return {WorkbenchActionKind::redraw};
    }
    active_menu_.clear();
    if (target.action == WorkbenchActionKind::scroll_to_tail) {
      timeline_scroll_ = timeline_max_scroll_;
      follow_tail_ = true;
      return {WorkbenchActionKind::redraw};
    }
    if (!target.file.empty()) {
      if (target.close_tab) {
        close_document(target.file);
        return {WorkbenchActionKind::redraw};
      }
      if (target.directory) {
        if (expanded_directories_.contains(target.file))
          expanded_directories_.erase(target.file);
        else
          expanded_directories_.insert(target.file);
        refresh_files(last_filter_);
        return {WorkbenchActionKind::redraw};
      }
      open_document(target.file);
      return {WorkbenchActionKind::redraw};
    }
    return {target.action, target.value, target.index};
  }
  if (!active_menu_.empty()) {
    active_menu_.clear();
    return {WorkbenchActionKind::redraw};
  }
  return {};
}

} // namespace tokmon::desktop
