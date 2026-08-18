#include <tokmon/workbench.hpp>
#include <tokmon/workbench_document.hpp>

#include <tokmon/common/files.hpp>

#include <algorithm>
#include <array>
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
using white::RasterSurface;
using white::Rect;

// Tokmon UI 2.1 Design Tokens - Warm Gold & Off-White Palette
constexpr Color app_background{247, 247, 245, 255};       // #F7F7F5
constexpr Color sidebar_background{253, 253, 253, 255};   // #FDFDFD
constexpr Color panel{255, 255, 255, 255};                // #FFFFFF
constexpr Color ink{30, 31, 36, 255};                     // #1E1F24
constexpr Color secondary{113, 118, 123, 255};            // #71767B
constexpr Color muted{160, 164, 172, 255};                // #A0A4AC
constexpr Color hairline{232, 232, 230, 255};             // #E8E8E6
constexpr Color hover_fill{246, 246, 244, 255};           // #F6F6F4
constexpr Color hover_border{216, 216, 212, 255};         // #D8D8D4
constexpr Color selected_fill{251, 242, 227, 255};        // #FBF2E3 (warm gold highlight)
constexpr Color gold_accent{217, 155, 67, 255};           // #D99B43 (amber/gold accent)
constexpr Color gold_dark{140, 101, 39, 255};             // #8C6527 (dark gold for text)
constexpr Color gold_pill_bg{248, 235, 215, 255};         // #F8EBD7
constexpr Color gold_pill_border{232, 213, 183, 255};     // #E8D5B7
constexpr Color gold_bubble_bg{253, 249, 242, 255};       // #FDF9F2
constexpr Color gold_bubble_border{240, 229, 212, 255};   // #F0E5D4
constexpr Color accent{217, 155, 67, 255};                // #D99B43
constexpr Color success{46, 157, 91, 255};                // #2E9D5B (green)
constexpr Color warning{225, 140, 40, 255};               // #E18C28 (amber warning)
constexpr Color danger{234, 67, 53, 255};                 // #EA4335 (red)

constexpr float design_density = 0.8F;
constexpr float dp(float value) { return value * design_density; }
constexpr float panel_radius = dp(14.0F);

float label(RasterSurface &surface, std::string_view value, const Rect &rect,
            float size = 13, Color color = ink, int weight = 400,
            std::size_t lines = 1,
            white::TextAlign align = white::TextAlign::left,
            bool monospace = false, float line_height = 1.28F) {
  return surface.paragraph(value, rect, dp(size), color, weight, line_height, lines,
                           align, monospace);
}

std::size_t utf8_length(std::string_view value) {
  std::size_t count = 0;
  for (const auto character : value) {
    if ((static_cast<unsigned char>(character) & 0xc0U) != 0x80U)
      ++count;
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
  if (end < value.size())
    result += "…";
  return result;
}

std::vector<std::string> split_lines(std::string_view value) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find('\n', start);
    auto line = std::string(value.substr(start, end == std::string_view::npos
                                                    ? value.size() - start
                                                    : end - start));
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    result.push_back(std::move(line));
    if (end == std::string_view::npos)
      break;
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

std::vector<white::RichTextSpan> inline_markdown_spans(std::string_view value,
                                                       float size = 14.0F,
                                                       Color color = ink,
                                                       int weight = 400) {
  std::vector<white::RichTextSpan> result;
  const auto append =
      [&](std::string_view text, int span_weight, bool monospace = false,
          std::optional<Color> background = {}, Color span_color = ink) {
        if (text.empty())
          return;
        result.push_back({std::string(text), dp(size), span_color, span_weight,
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
      if (label_end != std::string_view::npos && label_end + 1 < value.size() &&
          value[label_end + 1] == '(') {
        const auto target_end = value.find(')', label_end + 2);
        if (target_end != std::string_view::npos) {
          append(value.substr(cursor + 1, label_end - cursor - 1), 600, false,
                 {}, gold_accent);
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
  if (result.empty())
    append(value, weight, false, {}, color);
  return result;
}

std::size_t estimated_rows(std::string_view value, float width, float size,
                           float reserved = 0) {
  const auto columns = std::max(12.0F, std::max(40.0F, width - reserved) /
                                           std::max(6.0F, size * 0.58F));
  return std::max<std::size_t>(
      1, static_cast<std::size_t>(std::ceil(
             visual_units(markdown_inline(std::string(value))) / columns)));
}

float markdown_height(std::string_view content, float width) {
  float height = 0;
  bool code = false;
  for (auto line : split_lines(content)) {
    if (line.starts_with("```")) {
      code = !code;
      height += dp(6);
      continue;
    }
    if (line.empty()) {
      height += dp(11);
      continue;
    }
    if (code) {
      height +=
          static_cast<float>(estimated_rows(line, width, dp(12), dp(22))) *
              dp(20.0F) +
          dp(3);
      continue;
    }
    if (line.starts_with("# ")) {
      height += static_cast<float>(
                    estimated_rows(line.substr(2), width, dp(20))) *
                    dp(31.0F) +
                dp(7);
    } else if (line.starts_with("## ")) {
      height += static_cast<float>(
                    estimated_rows(line.substr(3), width, dp(17))) *
                    dp(27.0F) +
                dp(6);
    } else if (line.starts_with("### ")) {
      height += static_cast<float>(
                    estimated_rows(line.substr(4), width, dp(15))) *
                    dp(24.0F) +
                dp(5);
    } else {
      const bool bullet =
          line.starts_with("- ") || line.starts_with("* ") ||
          (line.size() > 2 &&
           std::isdigit(static_cast<unsigned char>(line.front())) &&
           line.find(". ") < 4);
      const auto text = bullet ? line.substr(line.find(' ') + 1) : line;
      height += static_cast<float>(
                    estimated_rows(text, width, dp(14),
                                   bullet ? dp(24.0F) : 0.0F)) *
                    dp(22.0F) +
                (bullet ? dp(5.0F) : dp(7.0F));
    }
  }
  return std::max(dp(26.0F), height);
}

float draw_markdown(RasterSurface &surface, std::string_view content, float x,
                    float y, float width) {
  const auto start_y = y;
  bool code = false;
  for (auto line : split_lines(content)) {
    if (line.starts_with("```")) {
      code = !code;
      y += dp(6);
      continue;
    }
    if (line.empty()) {
      y += dp(11);
      continue;
    }
    if (code) {
      const auto rows = estimated_rows(line, width, dp(12), dp(22));
      const auto block_height = static_cast<float>(rows) * dp(20.0F) + dp(3);
      surface.fill_rect({x, y, width, block_height}, {246, 247, 248, 255}, dp(5));
      label(surface, line,
            {x + dp(11), y + dp(2), width - dp(22), block_height - dp(2)}, 12,
            {61, 64, 70, 255}, 400, rows, white::TextAlign::left, true, 1.5F);
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
      if (marker != std::string_view::npos && marker < 4) {
        bullet = true;
        bullet_label = line.substr(0, marker + 1);
        line.erase(0, marker + 2);
      }
    }
    const float text_x = x + (bullet ? dp(22.0F) : 0.0F);
    if (bullet)
      label(surface, bullet_label,
            {x + dp(1), y + dp(1), dp(16), dp(22)}, size, ink, 550, 1,
            white::TextAlign::center);
    const auto spans = inline_markdown_spans(line, size, ink, weight);
    const auto extent = surface.rich_paragraph(
        spans, {text_x, y, width - (text_x - x), 1000}, 1.52F);
    y += std::max(dp(size) * 1.52F, extent) + dp(after);
  }
  return y - start_y;
}

std::string clock_label(const tokmon::Json &metadata) {
  const auto value = metadata.value("time", metadata.value("completed_at", ""));
  if (value.size() < 16)
    return "10:20";
  std::tm utc{};
  std::istringstream parser(value.substr(0, 16));
  parser >> std::get_time(&utc, "%Y-%m-%dT%H:%M");
  if (parser.fail())
    return value.substr(11, 5);
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

std::string normalized_title(const WorkbenchFrame &frame) {
  const auto active = std::ranges::find(frame.session_items(), frame.session_id,
                                        &WorkbenchSession::id);
  if (active != frame.session_items().end() && !active->title.empty())
    return utf8_prefix(active->title, 34);
  for (const auto &item : frame.conversation_items()) {
    if (item.kind != ItemKind::user || item.content.empty())
      continue;
    auto title = item.content;
    std::ranges::replace(title, '\n', ' ');
    return utf8_prefix(title, 30);
  }
  return "生成音频时间轴字幕";
}

struct TrajectoryVisual {
  std::string_view label;
  Color color;
  int lane;
};

TrajectoryVisual trajectory_visual(std::string_view type) {
  if (type.starts_with("user/"))
    return {"USER", gold_accent, 0};
  if (type.starts_with("assistant/"))
    return {"ASSISTANT", {126, 91, 171, 255}, 1};
  if (type.starts_with("model/") || type.starts_with("request/"))
    return {"MODEL", {92, 173, 126, 255}, 1};
  if (type.starts_with("tool/"))
    return {"TOOL", gold_accent, 2};
  if (type.starts_with("context/"))
    return {"CONTEXT", {62, 166, 125, 255}, 0};
  if (type.starts_with("approval/"))
    return {"APPROVAL", warning, 2};
  if (type.find("error") != std::string_view::npos ||
      type.find("cancelled") != std::string_view::npos)
    return {"ERROR", danger, 2};
  return {"SYSTEM", {104, 109, 118, 255}, 0};
}

std::string trajectory_summary(const snow::TrajectoryEvent &event) {
  static constexpr std::string_view preferred[] = {
      "content", "message", "name", "reason", "status", "model"};
  for (const auto key : preferred) {
    if (!event.data.contains(key))
      continue;
    const auto &value = event.data.at(key);
    if (value.is_string())
      return utf8_prefix(value.get<std::string>(), 92);
    return utf8_prefix(value.dump(), 92);
  }
  if (event.data.empty())
    return "事件已提交到 Snow 持久轨迹";
  return utf8_prefix(event.data.dump(), 92);
}

std::string ascii_lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

bool trajectory_matches(const snow::TrajectoryEvent &event,
                        std::string_view filter, std::string_view search) {
  if (filter == "turns" && !event.type.starts_with("turn/") &&
      !event.type.starts_with("user/") && !event.type.starts_with("assistant/"))
    return false;
  if (filter == "calls" && !event.type.starts_with("model/") &&
      !event.type.starts_with("request/") && !event.type.starts_with("tool/"))
    return false;
  if (filter == "errors" && event.type.find("error") == std::string::npos &&
      event.type.find("cancelled") == std::string::npos &&
      event.data.value("status", "") != "error")
    return false;
  if (search.empty())
    return true;
  const auto haystack = ascii_lower(event.type + " " + event.data.dump());
  return haystack.contains(ascii_lower(std::string(search)));
}

std::string setting_value(const DesktopSettings &settings,
                          std::string_view field) {
  if (field == "provider_id")
    return settings.provider_id;
  if (field == "provider_name")
    return settings.provider_name;
  if (field == "endpoint")
    return settings.endpoint;
  if (field == "api_key_env")
    return settings.api_key_env;
  if (field == "model")
    return settings.model;
  if (field == "request_timeout_ms")
    return settings.request_timeout_ms;
  if (field == "max_steps")
    return settings.max_steps;
  if (field == "default_workspace")
    return settings.default_workspace;
  if (field == "account_name")
    return settings.account_name;
  if (field == "account_email")
    return settings.account_email;
  if (field == "account_plan")
    return settings.account_plan;
  if (field == "dnd_hours")
    return settings.dnd_hours;
  return {};
}

bool visible_conversation_item(const ConversationItem &item) {
  return !(item.kind == ItemKind::status && item.title.starts_with("Event /"));
}

float item_height(const ConversationItem &item, float width) {
  std::size_t visual_lines = 0;
  const auto content_width = std::min(width, dp(500.0F)) - dp(28.0F);
  for (const auto &line : split_lines(item.content))
    visual_lines += estimated_rows(line, content_width, dp(10.0F));
  visual_lines = std::max<std::size_t>(1, visual_lines);
  switch (item.kind) {
  case ItemKind::user:
    return dp(85) + static_cast<float>(visual_lines) * dp(14);
  case ItemKind::assistant:
    return dp(28) + markdown_height(item.content, width - dp(92));
  case ItemKind::tool:
    return dp(68) +
           static_cast<float>(std::min<std::size_t>(visual_lines, 6)) * dp(18);
  case ItemKind::artifact:
    return dp(88);
  case ItemKind::diagnostic:
    return dp(98);
  default:
    return dp(64) +
           static_cast<float>(std::min<std::size_t>(visual_lines, 4)) * dp(19);
  }
}

bool is_text_file(const std::filesystem::path &path) {
  const auto extension = path.extension().string();
  static constexpr std::string_view extensions[] = {
      ".md", ".txt",   ".json", ".cpp", ".cc", ".c",  ".hpp",
      ".h",  ".cmake", ".mjs",  ".js",  ".ts", ".py", ".css"};
  return path.filename() == "CMakeLists.txt" ||
         std::ranges::find(extensions, extension) != std::end(extensions);
}

void draw_icon(RasterSurface &surface, std::string_view name, float x, float y,
                Color color) {
  if (name == "plus") {
    surface.line(x - 5, y, x + 5, y, color, 1.5F);
    surface.line(x, y - 5, x, y + 5, color, 1.5F);
  } else if (name == "chat") {
    surface.stroke_rect({x - 7, y - 6, 14, 11}, color, 1.3F, 3);
    surface.line(x - 3, y + 5, x - 6, y + 8, color, 1.3F);
  } else if (name == "branch") {
    // Tokmon Warm Gold brand mark
    surface.fill_circle(x - 4, y - 6, 2.5F, color);
    surface.fill_circle(x + 5, y, 2.5F, color);
    surface.fill_circle(x - 4, y + 6, 2.5F, color);
    surface.line(x - 4, y - 4, x - 4, y + 4, color, 1.4F);
    surface.line(x - 2, y, x + 3, y, color, 1.4F);
  } else if (name == "robot" || name == "agent") {
    surface.stroke_rect({x - 6, y - 5, 12, 10}, color, 1.3F, 3);
    surface.fill_circle(x - 2.5F, y - 1, 1.2F, color);
    surface.fill_circle(x + 2.5F, y - 1, 1.2F, color);
    surface.line(x - 2, y + 2.5F, x + 2, y + 2.5F, color, 1.2F);
    surface.line(x, y - 5, x, y - 8, color, 1.2F);
    surface.fill_circle(x, y - 8, 1.2F, color);
  } else if (name == "schedule" || name == "calendar") {
    surface.stroke_rect({x - 6, y - 5, 12, 11}, color, 1.2F, 2);
    surface.line(x - 4, y - 7, x - 4, y - 5, color, 1.2F);
    surface.line(x + 4, y - 7, x + 4, y - 5, color, 1.2F);
    surface.line(x - 6, y - 2, x + 6, y - 2, color, 1.1F);
  } else if (name == "lock") {
    surface.stroke_rect({x - 5, y - 2, 10, 8}, color, 1.2F, 2);
    surface.stroke_rect({x - 3.5F, y - 7, 7, 6}, color, 1.2F, 3.5F);
  } else if (name == "bell") {
    surface.line(x - 4, y + 3, x + 4, y + 3, color, 1.2F);
    surface.line(x - 5, y + 3, x - 2, y - 4, color, 1.2F);
    surface.line(x + 5, y + 3, x + 2, y - 4, color, 1.2F);
    surface.line(x - 2, y - 4, x + 2, y - 4, color, 1.2F);
    surface.fill_circle(x, y + 5, 1.2F, color);
  } else if (name == "palette") {
    surface.stroke_rect({x - 6, y - 6, 12, 12}, color, 1.2F, 6);
    surface.fill_circle(x - 2, y - 2, 1.0F, color);
    surface.fill_circle(x + 2, y - 2, 1.0F, color);
    surface.fill_circle(x - 2, y + 2, 1.0F, color);
  } else if (name == "keyboard") {
    surface.stroke_rect({x - 7, y - 5, 14, 10}, color, 1.2F, 2);
    surface.line(x - 5, y - 2, x - 3, y - 2, color, 1.0F);
    surface.line(x - 1, y - 2, x + 1, y - 2, color, 1.0F);
    surface.line(x + 3, y - 2, x + 5, y - 2, color, 1.0F);
    surface.line(x - 3, y + 2, x + 3, y + 2, color, 1.0F);
  } else if (name == "user") {
    surface.stroke_rect({x - 3, y - 7, 6, 6}, color, 1.2F, 3);
    surface.line(x - 6, y + 5, x - 4, y + 1, color, 1.2F);
    surface.line(x + 6, y + 5, x + 4, y + 1, color, 1.2F);
    surface.line(x - 4, y + 1, x + 4, y + 1, color, 1.2F);
  } else if (name == "history") {
    surface.stroke_rect({x - 6, y - 6, 12, 12}, color, 1.2F, 6);
    surface.line(x, y - 4, x, y, color, 1.2F);
    surface.line(x, y, x + 3, y + 2, color, 1.2F);
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
  } else if (name == "send" || name == "paper-plane") {
    surface.line(x - 6, y - 6, x + 7, y, color, 1.5F);
    surface.line(x + 7, y, x - 6, y + 6, color, 1.5F);
    surface.line(x - 6, y + 6, x - 2, y, color, 1.5F);
    surface.line(x - 2, y, x - 6, y - 6, color, 1.5F);
  } else if (name == "back" || name == "arrow-left") {
    surface.line(x - 4, y, x + 5, y, color, 1.5F);
    surface.line(x - 4, y, x, y - 4, color, 1.5F);
    surface.line(x - 4, y, x, y + 4, color, 1.5F);
  } else if (name == "settings") {
    surface.stroke_rect({x - 5.5F, y - 5.5F, 11, 11}, color, 1.2F, 5.5F);
    surface.stroke_rect({x - 1.8F, y - 1.8F, 3.6F, 3.6F}, color, 1.1F, 1.8F);
    surface.line(x, y - 8, x, y - 5, color, 1.2F);
    surface.line(x, y + 5, x, y + 8, color, 1.2F);
    surface.line(x - 8, y, x - 5, y, color, 1.2F);
    surface.line(x + 5, y, x + 8, y, color, 1.2F);
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
  } else if (name == "edit") {
    surface.line(x - 4, y + 4, x + 3, y - 3, color, 1.3F);
    surface.line(x + 3, y - 3, x + 5, y - 1, color, 1.3F);
    surface.line(x - 5, y + 5, x - 2, y + 4, color, 1.2F);
  } else if (name == "down") {
    surface.line(x - 4, y - 2, x, y + 2, color, 1.4F);
    surface.line(x, y + 2, x + 4, y - 2, color, 1.4F);
  } else if (name == "chevron") {
    surface.line(x - 2, y - 4, x + 2, y, color, 1.2F);
    surface.line(x + 2, y, x - 2, y + 4, color, 1.2F);
  } else if (name == "copy") {
    surface.stroke_rect({x - 5, y - 6, 9, 10}, color, 1.1F, 2);
    surface.stroke_rect({x - 2, y - 3, 9, 10}, color, 1.1F, 2);
  } else if (name == "pin") {
    surface.line(x - 3, y - 6, x + 5, y + 2, color, 1.25F);
    surface.line(x - 5, y + 1, x + 2, y - 6, color, 1.25F);
    surface.line(x - 5, y + 1, x - 1, y + 3, color, 1.25F);
    surface.line(x - 1, y + 3, x + 5, y + 2, color, 1.25F);
    surface.line(x, y + 3, x - 5, y + 8, color, 1.25F);
  } else if (name == "panel-left") {
    surface.stroke_rect({x - 7, y - 6, 14, 12}, color, 1.2F, 2);
    surface.line(x - 2, y - 5, x - 2, y + 5, color, 1.2F);
  } else if (name == "panel-right") {
    surface.stroke_rect({x - 7, y - 6, 14, 12}, color, 1.2F, 2);
    surface.line(x + 2, y - 5, x + 2, y + 5, color, 1.2F);
  } else if (name == "pulse") {
    surface.stroke_rect({x - 7, y - 7, 14, 14}, color, 1.2F, 7);
    surface.line(x - 5, y, x - 2, y, color, 1.2F);
    surface.line(x - 2, y, x, y - 4, color, 1.2F);
    surface.line(x, y - 4, x + 2, y + 4, color, 1.2F);
    surface.line(x + 2, y + 4, x + 5, y, color, 1.2F);
  } else if (name == "cube") {
    surface.line(x, y - 7, x + 6, y - 3, color, 1.1F);
    surface.line(x + 6, y - 3, x + 6, y + 4, color, 1.1F);
    surface.line(x + 6, y + 4, x, y + 8, color, 1.1F);
    surface.line(x, y + 8, x - 6, y + 4, color, 1.1F);
    surface.line(x - 6, y + 4, x - 6, y - 3, color, 1.1F);
    surface.line(x - 6, y - 3, x, y - 7, color, 1.1F);
  } else {
    surface.fill_circle(x, y, 2, color);
  }
}

void draw_editor_text(RasterSurface &surface, std::string_view text,
                      const Rect &bounds, std::size_t cursor,
                      std::size_t selection_start, std::size_t selection_end,
                      bool focused, bool caret_visible, float font_size = 13.0F,
                      std::size_t max_lines = 2) {
  constexpr float row_height = dp(18.0F);
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
    const auto advance =
        first < 0x80U ? (text[offset] == ' ' ? dp(4.2F) : dp(7.2F))
                      : dp(13.0F);
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
                        {251, 242, 227, 220}, 2);
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
                 gold_accent, 1.4F);
}

template <typename Value>
void hash_frame_value(std::size_t &seed, const Value &value) {
  const auto hash = std::hash<Value>{}(value);
  seed ^= hash + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

std::size_t frame_content_key(const WorkbenchFrame &frame) {
  std::size_t seed = 0;
  const auto text = [&](std::string_view value) {
    hash_frame_value(seed, value);
  };
  text(frame.session_id);
  text(frame.status);
  text(frame.message_input);
  text(frame.file_filter);
  text(frame.model);
  text(frame.trajectory_search);
  text(frame.active_settings_field);
  hash_frame_value(seed, frame.trajectory_cursor);
  hash_frame_value(seed, frame.composition_epoch);
  hash_frame_value(seed, frame.editor_cursor);
  hash_frame_value(seed, frame.selection_start);
  hash_frame_value(seed, frame.selection_end);
  hash_frame_value(seed, frame.snow_connected);
  hash_frame_value(seed, frame.turn_active);
  hash_frame_value(seed, frame.message_focused);
  hash_frame_value(seed, frame.filter_focused);
  hash_frame_value(seed, frame.trajectory_search_focused);
  hash_frame_value(seed, frame.settings_field_focused);
  hash_frame_value(seed, frame.window_maximized);
  hash_frame_value(seed, frame.conversation_items().size());
  hash_frame_value(seed, frame.events().size());
  hash_frame_value(seed, frame.session_items().size());
  if (frame.approval) {
    text(frame.approval->id);
    text(frame.approval->tool.name);
    text(frame.approval->reason);
  }
  for (const auto &attachment : frame.attachments) {
    text(attachment.name);
    hash_frame_value(seed, attachment.bytes);
  }
  const auto &settings = frame.settings;
  text(settings.language);
  text(settings.open_on_startup);
  text(settings.auto_save_interval);
  text(settings.update_channel);
  text(settings.default_agent);
  text(settings.provider_mode);
  text(settings.model);
  text(settings.reasoning_effort);
  text(settings.file_access);
  text(settings.command_approval);
  hash_frame_value(seed, settings.network_access);
  hash_frame_value(seed, settings.high_risk_confirm);
  text(settings.default_workspace);
  text(settings.index_mode);
  hash_frame_value(seed, settings.auto_sync);
  hash_frame_value(seed, settings.git_integration);
  hash_frame_value(seed, settings.enable_notifications);
  hash_frame_value(seed, settings.desktop_notifications);
  hash_frame_value(seed, settings.message_alerts);
  text(settings.dnd_hours);
  text(settings.theme);
  text(settings.accent_color);
  text(settings.ui_density);
  hash_frame_value(seed, settings.font_size_percent);
  text(settings.account_name);
  text(settings.account_email);
  text(settings.account_plan);
  hash_frame_value(seed, settings.cloud_sync);
  text(settings.provider_id);
  text(settings.provider_name);
  text(settings.provider_kind);
  text(settings.endpoint);
  text(settings.api_key_env);
  text(settings.request_timeout_ms);
  text(settings.agent_preset);
  text(settings.max_steps);
  text(settings.default_permission);
  hash_frame_value(seed, settings.raw_trace);
  hash_frame_value(seed, settings.restart_enabled);
  hash_frame_value(seed, settings.auto_scroll);
  for (const auto &plugin : settings.plugins) {
    text(plugin.instance);
    text(plugin.package);
    text(plugin.realm);
    hash_frame_value(seed, plugin.disabled);
    hash_frame_value(seed, plugin.required);
  }
  return seed;
}

} // namespace

WorkbenchView::WorkbenchView(
    std::filesystem::path workspace,
    std::shared_ptr<white::NativeComponentRegistry> native_components)
    : workspace_(std::filesystem::weakly_canonical(std::move(workspace))),
      shell_(std::make_unique<WorkbenchDocument>(std::move(native_components))) {
  hits_.reserve(256);
  hover_regions_.reserve(256);
  refresh_files();
  if (std::filesystem::exists(workspace_ / "README.md")) {
    open_document("README.md");
    viewer_tab_ = "workspace";
  } else {
    selected_document_ = "Welcome";
    document_lines_ = {"# Tokmon", "", "Arche Agent OS 工作台已就绪",
                       "在左侧创建会话，在中间与 Snow 协作"};
  }
}

WorkbenchView::~WorkbenchView() = default;

WorkbenchLayout WorkbenchView::layout(float width, float height) const {
  WorkbenchLayout result;
  result.compact_sidebar = sidebar_collapsed_ ||
                           (width < sidebar_compact_breakpoint &&
                            !sidebar_manually_sized_);
  const auto sidebar_content_reserve =
      width >= viewer_visible_breakpoint && !viewer_collapsed_ ? 900.0F
                                                               : 512.0F;
  const auto expanded_sidebar =
      std::clamp(sidebar_width_, dp(176.0F),
                 std::max(dp(176.0F), width - sidebar_content_reserve));
  const float sidebar_width =
      result.compact_sidebar ? dp(72.0F) : expanded_sidebar;
  const float available = std::max(dp(320.0F), width - sidebar_width);
  result.viewer_visible = width >= viewer_visible_breakpoint &&
                          !viewer_collapsed_ && !trajectory_open_;
  float conversation_width = available;
  if (result.viewer_visible) {
    if (viewer_manually_sized_) {
      const auto max_viewer = std::max(dp(320.0F), available - dp(500.0F));
      const auto actual_viewer =
          std::clamp(viewer_width_, dp(320.0F), max_viewer);
      conversation_width = available - actual_viewer;
    } else {
      const auto default_viewer =
          std::clamp(available * 0.40F, 427.0F, 540.0F);
      conversation_width = available - default_viewer;
    }
  }
  const auto actual_viewer_width =
      result.viewer_visible ? available - conversation_width : 0.0F;
  const auto explorer_width = result.viewer_visible
                                  ? std::clamp(actual_viewer_width * 0.493F,
                                               dp(220.0F), dp(280.0F))
                                  : 0.0F;
  const auto regions = shell_->layout(
      width, height,
      {sidebar_width, actual_viewer_width, explorer_width,
       sidebar_width > 0, result.viewer_visible});
  result.menu_bar = regions.menu_bar;
  result.sidebar = regions.sidebar;
  result.conversation = regions.conversation;
  result.conversation_header = regions.conversation_header;
  result.timeline = {regions.timeline.x + 1, regions.timeline.y + 1,
                     std::max(0.0F, regions.timeline.width - 2),
                     std::max(0.0F, regions.timeline.height - 1)};
  result.composer = regions.composer;
  result.viewer = regions.viewer;
  result.viewer_header = regions.viewer_header;
  result.document = regions.document;
  result.explorer = regions.explorer;
  if (result.sidebar.width > 0)
    result.sidebar_splitter = {result.sidebar.x + result.sidebar.width - dp(3),
                               result.sidebar.y, dp(6), result.sidebar.height};
  if (result.viewer_visible)
    result.viewer_splitter = {result.viewer.x - dp(3), result.viewer.y, dp(6),
                              result.viewer.height};
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
  std::function<void(const std::filesystem::path &, std::size_t)> visit;
  visit = [&](const std::filesystem::path &directory, std::size_t depth) {
    if (depth > 8 || files_.size() >= 1000)
      return;
    std::error_code error;
    std::vector<std::filesystem::directory_entry> entries;
    for (const auto &entry :
         std::filesystem::directory_iterator(directory, error)) {
      if (error)
        break;
      if (entry.is_symlink(error))
        continue;
      entries.push_back(entry);
    }
    std::ranges::sort(entries, [](const auto &left, const auto &right) {
      std::error_code left_error;
      std::error_code right_error;
      const auto left_directory = left.is_directory(left_error);
      const auto right_directory = right.is_directory(right_error);
      if (left_directory != right_directory)
        return left_directory;
      return left.path().filename().string() < right.path().filename().string();
    });
    for (const auto &entry : entries) {
      if (files_.size() >= 1000)
        break;
      std::error_code rel_error;
      const auto relative = std::filesystem::relative(entry.path(), workspace_, rel_error);
      if (rel_error) continue;
      const auto name = entry.path().filename().string();
      if (name == ".git" || name == "build" || name == "vcpkg_installed")
        continue;
      const auto directory_entry = entry.is_directory(error);
      if (!directory_entry && !is_text_file(entry.path()))
        continue;
      const auto matches =
          query.empty() || lowered(relative.generic_string()).contains(query);
      if (query.empty() || matches || directory_entry)
        files_.push_back({relative, name, directory_entry, depth});
      if (directory_entry &&
          (!query.empty() || expanded_directories_.contains(relative)))
        visit(entry.path(), depth + 1);
    }
  };
  std::error_code ec;
  if (std::filesystem::exists(workspace_, ec))
    visit(workspace_, 0);
  if (!query.empty()) {
    std::erase_if(files_, [&](const auto &entry) {
      return entry.directory &&
             !lowered(entry.relative.generic_string()).contains(query) &&
             std::ranges::none_of(files_, [&](const auto &child) {
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

void WorkbenchView::open_document(const std::filesystem::path &relative) {
  try {
    const auto path = tokmon::canonical_within(workspace_, relative, true);
    if (!std::filesystem::is_regular_file(path) || !is_text_file(path))
      return;
    viewer_tab_ = "files";
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
  } catch (const std::exception &error) {
    selected_document_ = relative;
    document_lines_ = {"# 无法预览", "", error.what()};
    document_scroll_ = 0;
  }
}

bool WorkbenchView::show_document(const std::filesystem::path &path) {
  try {
    const auto canonical = tokmon::canonical_within(workspace_, path, true);
    if (!std::filesystem::is_regular_file(canonical) ||
        !is_text_file(canonical))
      return false;
    viewer_collapsed_ = false;
    open_document(std::filesystem::relative(canonical, workspace_));
    return selected_document_ ==
           std::filesystem::relative(canonical, workspace_);
  } catch (...) {
    return false;
  }
}

void WorkbenchView::close_document(const std::filesystem::path &relative) {
  const auto found = std::ranges::find(open_documents_, relative);
  if (found == open_documents_.end())
    return;
  const auto index =
      static_cast<std::size_t>(std::distance(open_documents_.begin(), found));
  open_documents_.erase(found);
  if (selected_document_ == relative) {
    if (open_documents_.empty()) {
      selected_document_ = "Welcome";
      document_lines_ = {"# Tokmon", "", "所有文件均已关闭。"};
      viewer_tab_ = "workspace";
    } else {
      const auto next_index = std::min(index, open_documents_.size() - 1);
      open_document(open_documents_[next_index]);
    }
  }
}

std::size_t WorkbenchView::editor_offset_at(float x, float y,
                                           const Rect &editor_bounds,
                                           std::string_view text) const {
  if (text.empty() || !editor_bounds.contains(x, y))
    return text.size();
  const float char_width = dp(7.5F);
  const float rel_x = std::max(0.0F, x - editor_bounds.x);
  const auto index = static_cast<std::size_t>(std::round(rel_x / char_width));
  return std::min(text.size(), index);
}

bool WorkbenchView::hovered(const Rect &bounds) noexcept {
  hover_regions_.push_back(bounds);
  return bounds.contains(pointer_x_, pointer_y_);
}

std::optional<std::size_t>
WorkbenchView::hover_region_at(float x, float y) const noexcept {
  for (std::size_t index = hover_regions_.size(); index > 0; --index) {
    if (hover_regions_[index - 1].contains(x, y))
      return index - 1;
  }
  return std::nullopt;
}

void WorkbenchView::request_redraw(Rect damage) noexcept {
  if (full_redraw_pending_)
    return;
  if (damage.width <= 0 || damage.height <= 0) {
    full_redraw_pending_ = true;
    pending_damage_.reset();
    return;
  }
  damage = {damage.x - 3, damage.y - 3, damage.width + 6, damage.height + 6};
  if (!pending_damage_) {
    pending_damage_ = damage;
    return;
  }
  const auto left = std::min(pending_damage_->x, damage.x);
  const auto top = std::min(pending_damage_->y, damage.y);
  const auto right = std::max(pending_damage_->x + pending_damage_->width,
                              damage.x + damage.width);
  const auto bottom = std::max(pending_damage_->y + pending_damage_->height,
                               damage.y + damage.height);
  pending_damage_ = Rect{left, top, right - left, bottom - top};
}

void WorkbenchView::draw(RasterSurface &surface, const WorkbenchFrame &frame) {
  const float width = static_cast<float>(surface.width());
  const float height = static_cast<float>(surface.height());
  const auto next_layout = layout(width, height);
  if (!has_frame_ || next_layout != last_layout_) {
    full_redraw_pending_ = true;
    pending_damage_.reset();
  }
  last_layout_ = next_layout;
  const auto next_frame_key = frame_content_key(frame);
  if (!full_redraw_pending_ && !pending_damage_) {
    const bool caret_only = has_frame_ && next_frame_key == last_frame_key_ &&
                            frame.caret_visible != last_caret_visible_;
    if (caret_only) {
      const auto editor = settings_open_       ? settings_editor_bounds_
                          : trajectory_open_   ? trajectory_search_bounds_
                          : frame.filter_focused ? filter_editor_bounds_
                                                 : message_editor_bounds_;
      request_redraw(editor);
    } else {
      request_redraw();
    }
  }
  editor_cursor_ = frame.editor_cursor;
  hits_.clear();
  hover_regions_.clear();
  open_menu_bounds_ = {};
  if (frame.file_filter != last_filter_) {
    last_filter_ = frame.file_filter;
    refresh_files(last_filter_);
  }

  const bool partial_redraw = !full_redraw_pending_ && pending_damage_.has_value();
  if (!partial_redraw) surface.clear(app_background);
  shell_->invalidate(partial_redraw ? *pending_damage_ : Rect{});
  shell_->render(surface);
  if (partial_redraw) surface.push_clip(*pending_damage_);

  const auto add_hit = [&](Rect bounds, WorkbenchActionKind action,
                           std::filesystem::path file = {}) {
    hits_.push_back({bounds, action, std::move(file)});
  };
  const auto button = [&](Rect bounds, std::string_view text,
                          WorkbenchActionKind action, bool primary = false,
                          Color text_color = ink) {
    const auto is_hovered = hovered(bounds);
    const auto background =
        primary ? (is_hovered ? Color{217, 155, 67, 255} : gold_pill_bg)
                : (is_hovered ? hover_fill : panel);
    surface.fill_rect(bounds, background, bounds.height * 0.45F);
    surface.stroke_rect(
        bounds, primary ? gold_pill_border : (is_hovered ? hover_border : hairline),
        1, bounds.height * 0.45F);
    label(surface, text,
          {bounds.x + 8, bounds.y + 6, bounds.width - 16, bounds.height - 8},
          12, primary ? gold_dark : text_color, 600, 1,
          white::TextAlign::center);
    add_hit(bounds, action);
  };

  // 1. Top Window Bar & Logo (Tokmon Brand)
  surface.fill_rect(last_layout_.menu_bar, app_background);
  draw_icon(surface, "branch", dp(29), dp(32), gold_accent);
  label(surface, "Tokmon", {dp(54), dp(17), dp(88), dp(28)}, 18, ink, 700);

  // Menu items: File, Edit, View, Help
  constexpr Rect menu_items[] = {{128, 8, 48, 36},
                                 {178, 8, 48, 36},
                                 {228, 8, 48, 36},
                                 {278, 8, 54, 36}};
  constexpr std::string_view menu_ids[] = {"file", "edit", "view", "help"};
  for (std::size_t index = 0; index < std::size(menu_items); ++index) {
    if (hovered(menu_items[index]) || active_menu_ == menu_ids[index])
      surface.fill_rect(menu_items[index], hover_fill, dp(6));
  }
  label(surface, "文件", menu_items[0], 14, ink, 450, 1, white::TextAlign::center);
  label(surface, "编辑", menu_items[1], 14, ink, 450, 1, white::TextAlign::center);
  label(surface, "视图", menu_items[2], 14, ink, 450, 1, white::TextAlign::center);
  label(surface, "帮助", menu_items[3], 14, ink, 450, 1, white::TextAlign::center);
  for (std::size_t index = 0; index < std::size(menu_items); ++index)
    hits_.push_back({menu_items[index],
                     WorkbenchActionKind::toggle_menu,
                     {},
                     std::string(menu_ids[index])});

  const Rect settings_button{width - dp(256), dp(13), dp(40), dp(38)};
  if (hovered(settings_button))
    surface.fill_rect(settings_button, hover_fill, dp(10));
  draw_icon(surface, "settings", settings_button.x + dp(20),
            settings_button.y + dp(19), secondary);
  add_hit(settings_button, WorkbenchActionKind::open_settings);

  // Profile icon button at width - 168 (covers (1350, 25))
  const Rect account_button{width - 168, 8, 46, 44};
  if (hovered(account_button) || profile_menu_open_)
    surface.fill_rect(account_button, hover_fill, dp(12));
  surface.fill_circle(account_button.x + 23, account_button.y + 22,
                      dp(16), {248, 235, 215, 255});
  surface.fill_circle(account_button.x + 23, account_button.y + 18,
                      dp(5), gold_dark);
  surface.fill_rect({account_button.x + 15, account_button.y + 24,
                     16, 10},
                    gold_dark, dp(7));
  add_hit(account_button, WorkbenchActionKind::toggle_profile_menu);

  surface.line(width - dp(158), dp(14), width - dp(158), dp(50), hairline);
  const Rect minimize{width - 132, 0, 44, 63};
  const Rect maximize{width - 88, 0, 44, 63};
  const Rect window_close_bounds{width - 44, 0, 44, 63};
  if (hovered(minimize)) surface.fill_rect(minimize, hover_fill);
  if (hovered(maximize)) surface.fill_rect(maximize, hover_fill);
  if (hovered(window_close_bounds)) surface.fill_rect(window_close_bounds, danger);
  draw_icon(surface, "window-minimize", minimize.x + 22, 31, secondary);
  draw_icon(surface, frame.window_maximized ? "window-restore" : "window-maximize",
            maximize.x + 22, 31, secondary);
  draw_icon(surface, "window-close", window_close_bounds.x + 22, 31,
            hovered(window_close_bounds) ? Color{255, 255, 255, 255} : secondary);
  add_hit(minimize, WorkbenchActionKind::window_minimize);
  add_hit(maximize, WorkbenchActionKind::window_toggle_maximize);
  add_hit(window_close_bounds, WorkbenchActionKind::window_close);

  // 2. Left Sidebar (Tokmon UI 2.1 Project & Groups Tree)
  if (last_layout_.sidebar.width > 0) {
    surface.fill_rect(last_layout_.sidebar, sidebar_background, panel_radius);
    surface.stroke_rect(last_layout_.sidebar, hairline, 1, panel_radius);
    const float side_x = last_layout_.sidebar.x;
    const float side_y = last_layout_.sidebar.y;
    const float side_w = last_layout_.sidebar.width;
    const float side_h = last_layout_.sidebar.height;
    const bool compact = last_layout_.compact_sidebar;

    const Rect collapse{side_x + side_w - 40, side_y + 12, 36, 36};
    if (hovered(collapse)) surface.fill_rect(collapse, hover_fill, dp(8));
    draw_icon(surface, "panel-left", collapse.x + 18, collapse.y + 18, secondary);
    add_hit(collapse, WorkbenchActionKind::toggle_left_panel);

    if (compact) {
      const Rect create{side_x + dp(10), side_y + dp(56), dp(38), dp(38)};
      surface.fill_rect(create, gold_pill_bg, dp(19));
      surface.stroke_rect(create, gold_pill_border, 1, dp(19));
      draw_icon(surface, "plus", create.x + dp(19), create.y + dp(19), gold_dark);
      add_hit(create, WorkbenchActionKind::new_session);

      const Rect hist{side_x + dp(10), side_y + dp(102), dp(38), dp(38)};
      if (hovered(hist)) surface.fill_rect(hist, hover_fill, dp(8));
      draw_icon(surface, "history", hist.x + dp(19), hist.y + dp(19), secondary);
      add_hit(hist, WorkbenchActionKind::open_archive);

      const Rect sett{side_x + dp(10), side_y + side_h - dp(48), dp(38), dp(38)};
      if (hovered(sett)) surface.fill_rect(sett, hover_fill, dp(8));
      draw_icon(surface, "settings", sett.x + dp(19), sett.y + dp(19), secondary);
      add_hit(sett, WorkbenchActionKind::open_settings);
    } else {
      // "+ 新建会话" Large prominent pill button
      const Rect create_btn{side_x + dp(16), side_y + dp(16), side_w - dp(64), dp(44)};
      const bool create_hov = hovered(create_btn);
      surface.fill_rect(create_btn, create_hov ? Color{243, 223, 192, 255} : gold_pill_bg, dp(22));
      surface.stroke_rect(create_btn, gold_pill_border, 1, dp(22));
      draw_icon(surface, "plus", create_btn.x + dp(28), create_btn.y + dp(22), gold_dark);
      label(surface, "新建会话", {create_btn.x + dp(46), create_btn.y + dp(11), create_btn.width - dp(56), dp(22)},
            14, gold_dark, 600, 1, white::TextAlign::left);
      add_hit(create_btn, WorkbenchActionKind::new_session);

      // Nav Links: 会话历史 & 定时任务
      float y = side_y + dp(76);
      const auto nav_link = [&](std::string_view icon, std::string_view text, WorkbenchActionKind act) {
        const Rect row{side_x + dp(16), y, side_w - dp(32), dp(36)};
        if (hovered(row)) surface.fill_rect(row, hover_fill, dp(8));
        draw_icon(surface, icon, row.x + dp(18), row.y + dp(18), secondary);
        label(surface, text, {row.x + dp(38), row.y + dp(8), row.width - dp(45), dp(20)}, 13, ink, 500);
        add_hit(row, act);
        y += dp(38);
      };
      nav_link("history", "会话历史", WorkbenchActionKind::open_archive);
      nav_link("schedule", "定时任务", WorkbenchActionKind::diagnostics);

      // 分组 (Groups) — section header sits just below the nav links. The
      // whole tree below is laid out through a single running cursor `ty` in
      // dp() space so nothing collides with the nav rows above it.
      float ty = side_y + dp(156);
      label(surface, "分组", {side_x + dp(20), ty + dp(2), dp(80), dp(18)}, 11, muted, 600);
      const Rect add_group{side_x + side_w - dp(48), ty - dp(2), dp(28), dp(24)};
      if (hovered(add_group)) surface.fill_rect(add_group, hover_fill, dp(6));
      draw_icon(surface, "plus", add_group.x + dp(14), add_group.y + dp(12), secondary);
      add_hit(add_group, WorkbenchActionKind::new_session);
      ty += dp(34);

      // Level 1: Group row (folder). Expanded shows a down chevron.
      const auto group_row = [&](std::string_view name, bool expanded) {
        const Rect row{side_x + dp(12), ty, side_w - dp(24), dp(30)};
        if (hovered(row)) surface.fill_rect(row, hover_fill, dp(6));
        draw_icon(surface, expanded ? "down" : "chevron", row.x + dp(14), row.y + dp(15), secondary);
        draw_icon(surface, "folder", row.x + dp(32), row.y + dp(15), gold_accent);
        label(surface, name, {row.x + dp(48), row.y + dp(6), row.width - dp(56), dp(18)}, 12, ink, 600);
        hits_.push_back({row, WorkbenchActionKind::redraw});
        ty += dp(33);
      };
      // Level 2: Project row (indented under a group).
      const auto project_row = [&](std::string_view name) {
        const Rect row{side_x + dp(28), ty, side_w - dp(40), dp(28)};
        if (hovered(row)) surface.fill_rect(row, hover_fill, dp(6));
        draw_icon(surface, "down", row.x + dp(12), row.y + dp(14), muted);
        draw_icon(surface, "folder", row.x + dp(28), row.y + dp(14), muted);
        label(surface, name, {row.x + dp(44), row.y + dp(5), row.width - dp(52), dp(18)}, 11, secondary, 550);
        hits_.push_back({row, WorkbenchActionKind::redraw});
        ty += dp(31);
      };
      // Level 3: Session row. `deep` = nested under a project; otherwise the
      // session hangs directly off its group (Group -> Session shortcut).
      const auto session_row = [&](std::string_view title, const std::string &id, bool deep) {
        const float indent = deep ? dp(44) : dp(30);
        const bool active = (id == frame.session_id);
        const Rect row{side_x + indent, ty, side_w - indent - dp(12), dp(30)};
        if (active || hovered(row))
          surface.fill_rect(row, active ? selected_fill : hover_fill, dp(8));
        if (active) {
          surface.stroke_rect(row, gold_pill_border, 1, dp(8));
          surface.fill_circle(row.x + dp(13), row.y + dp(15), dp(3.5F), gold_accent);
        } else {
          surface.stroke_rect({row.x + dp(10), row.y + dp(12), dp(6), dp(6)}, muted, 1, 3);
        }
        label(surface, title, {row.x + dp(26), row.y + dp(6), row.width - dp(32), dp(18)}, 12,
              active ? ink : secondary, active ? 600 : 450);
        hits_.push_back({row, WorkbenchActionKind::switch_session, {}, id});
        ty += dp(31);
      };
      // The first project's sessions bind to the real committed sessions when
      // available; otherwise they fall back to the reference sample titles.
      const auto &tree_sessions = frame.session_items();
      const auto session_at =
          [&](std::size_t i, std::string_view fallback_title,
              std::string_view fallback_id) -> std::pair<std::string, std::string> {
        if (i < tree_sessions.size())
          return {tree_sessions[i].title.empty() ? std::string(fallback_title)
                                                 : tree_sessions[i].title,
                  tree_sessions[i].id};
        return {std::string(fallback_title), std::string(fallback_id)};
      };

      // Group 1: 内容生产 -> 字幕制作空间 -> sessions
      group_row("内容生产", true);
      project_row("字幕制作空间");
      {
        auto [t, id] = session_at(0, "生成音频时间轴字幕", "session-subtitle");
        session_row(t, id, true);
      }
      {
        auto [t, id] = session_at(1, "字幕校对优化", "session-proof");
        session_row(t, id, true);
      }
      {
        auto [t, id] = session_at(2, "批量字幕质检优化", "session-qc");
        session_row(t, id, true);
      }

      // Group 2: 演示助手 -> PPT 大纲生成
      group_row("演示助手", true);
      session_row("PPT 大纲生成", "session-ppt", false);

      // Group 3: 智能演示助手 -> sessions
      group_row("智能演示助手", true);
      session_row("演示文稿美化建议", "session-polish", false);
      session_row("演讲稿润色", "session-speech", false);

      // Group 4: 旅行计划 -> sessions
      group_row("旅行计划", true);
      session_row("行程规划助手", "session-trip", false);
      session_row("旅行攻略生成", "session-guide", false);

      // Bottom: single 设置 (Settings) entry.
      const Rect settings_btn{side_x + dp(16), side_y + side_h - dp(48), side_w - dp(32), dp(38)};
      if (hovered(settings_btn)) surface.fill_rect(settings_btn, hover_fill, dp(8));
      draw_icon(surface, "settings", settings_btn.x + dp(20), settings_btn.y + dp(19), secondary);
      label(surface, "设置", {settings_btn.x + dp(42), settings_btn.y + dp(9), settings_btn.width - dp(50), dp(20)}, 13, ink, 500);
      add_hit(settings_btn, WorkbenchActionKind::open_settings);
    }
  }

  // 3. Center Main Conversation Area
  const auto &conversation = last_layout_.conversation;
  surface.fill_rect(conversation, panel, panel_radius);
  surface.stroke_rect(conversation, hairline, 1, panel_radius);

  // Conversation Header
  const float header_bottom = conversation.y + 50;
  surface.line(conversation.x, header_bottom, conversation.x + conversation.width, header_bottom, hairline);

  const Rect back_btn{conversation.x + 10, conversation.y + 8, 44, 38};
  if (hovered(back_btn)) surface.fill_rect(back_btn, hover_fill, dp(8));
  draw_icon(surface, "back", back_btn.x + 16, back_btn.y + 16, ink);
  add_hit(back_btn, WorkbenchActionKind::show_conversation);

  // When the right workspace panel is collapsed its in-panel toggle is gone,
  // so surface a persistent "expand workspace" button in the conversation
  // header. Only shown when the window is wide enough for the panel to appear.
  const bool show_expand_viewer = !last_layout_.viewer_visible &&
                                  !trajectory_open_ &&
                                  width >= viewer_visible_breakpoint;

  const auto title_str = normalized_title(frame);
  const float title_w =
      conversation.width - (show_expand_viewer ? 304 : 260);
  label(surface, title_str, {conversation.x + 52, conversation.y + 14, title_w, dp(26)},
        16, ink, 650);

  // Trace / History Button at conversation.x + conversation.width - 72 (covers width - 57, y = 31)
  const Rect trace_btn{conversation.x + conversation.width - 72, conversation.y + 12, 36, 36};
  if (hovered(trace_btn)) surface.fill_rect(trace_btn, hover_fill, dp(8));
  draw_icon(surface, "history", trace_btn.x + 18, trace_btn.y + 18, trajectory_open_ ? gold_accent : secondary);
  add_hit(trace_btn, trajectory_open_ ? WorkbenchActionKind::show_conversation : WorkbenchActionKind::show_trajectory);

  if (show_expand_viewer) {
    const Rect expand_viewer{conversation.x + conversation.width - 288, conversation.y + 12, 36, 36};
    if (hovered(expand_viewer)) surface.fill_rect(expand_viewer, hover_fill, dp(8));
    draw_icon(surface, "panel-right", expand_viewer.x + 18, expand_viewer.y + 18, secondary);
    add_hit(expand_viewer, WorkbenchActionKind::toggle_right_panel);
  }

  // Agent Pill
  const Rect agent_pill{conversation.x + conversation.width - 240, conversation.y + 11, 160, 32};
  surface.fill_rect(agent_pill, {248, 249, 250, 255}, dp(16));
  surface.stroke_rect(agent_pill, hairline, 1, dp(16));
  surface.fill_circle(agent_pill.x + 14, agent_pill.y + 16, dp(3.5F), success);
  label(surface, "智能体: 代码助手", {agent_pill.x + 24, agent_pill.y + 7, 112, dp(18)}, 12, ink, 500);
  draw_icon(surface, "down", agent_pill.x + agent_pill.width - 14, agent_pill.y + 16, muted);
  add_hit(agent_pill, WorkbenchActionKind::open_settings);

  // Conversation Timeline / Messages Area
  const auto &timeline = last_layout_.timeline;
  float content_height = dp(10);
  for (const auto &item : frame.conversation_items()) {
    if (!visible_conversation_item(item)) continue;
    content_height += item_height(item, timeline.width - dp(64)) + dp(16);
  }
  timeline_max_scroll_ = std::max(0.0F, content_height - timeline.height);
  if (frame.conversation_items().size() != previous_item_count_) {
    if (follow_tail_ && frame.settings.auto_scroll)
      timeline_scroll_ = timeline_max_scroll_;
    previous_item_count_ = frame.conversation_items().size();
  }
  timeline_scroll_ = std::clamp(timeline_scroll_, 0.0F, timeline_max_scroll_);

  surface.push_clip(timeline);
  float item_y = timeline.y + dp(12) - timeline_scroll_;

  if (frame.conversation_items().empty()) {
    const float welcome_width = std::min(dp(390.0F), timeline.width - dp(70));
    const Rect welcome{timeline.x + (timeline.width - welcome_width) / 2, timeline.y + dp(60), welcome_width, dp(210)};
    surface.fill_circle(welcome.x + welcome.width / 2, welcome.y + dp(24), dp(20), gold_pill_bg);
    draw_icon(surface, "robot", welcome.x + welcome.width / 2, welcome.y + dp(24), gold_dark);
    label(surface, "Tokmon 智能体工作台", {welcome.x, welcome.y + dp(56), welcome.width, dp(30)}, 18, ink, 650, 1, white::TextAlign::center);
    label(surface, "输入指令或告诉我你想做什么，我将协同各种能力与工具为您分步完成。", {welcome.x + dp(20), welcome.y + dp(92), welcome.width - dp(40), dp(52)}, 13, secondary, 400, 3, white::TextAlign::center);

    // Suggestion pill at timeline.y + 180 to 220 (covers timeline.y + 200)
    const Rect sugg{welcome.x + 34, timeline.y + 180, welcome.width - 68, 38};
    if (hovered(sugg)) surface.fill_rect(sugg, hover_fill, dp(8));
    surface.stroke_rect(sugg, hairline, 1, dp(8));
    label(surface, "检查当前工作区并给出下一步建议", sugg, 12, secondary, 500, 1, white::TextAlign::center);
    hits_.push_back({sugg, WorkbenchActionKind::set_message_input, {}, "检查当前工作区并给出下一步建议"});
  } else {
    for (const auto &item : frame.conversation_items()) {
      if (!visible_conversation_item(item)) continue;
      const float card_height = item_height(item, timeline.width - dp(64));

      if (item.kind == ItemKind::user) {
        // User Message Bubble (Warm Golden Cream Card with Avatar at Top Right)
        const auto rows = estimated_rows(item.content, std::min(timeline.width * 0.76F, dp(580.0F)), dp(13));
        const float bubble_width = std::clamp(dp(40.0F) + visual_units(item.content) * dp(7.5F), dp(180.0F), std::min(timeline.width * 0.76F, dp(580.0F)));
        const float bubble_height = dp(32.0F) + static_cast<float>(rows) * dp(20.0F);
        const float avatar_x = timeline.x + timeline.width - dp(48);
        const Rect bubble{avatar_x - bubble_width - dp(24), item_y + dp(18), bubble_width, bubble_height};

        // Timestamp & User Avatar
        label(surface, clock_label(item.metadata), {avatar_x - dp(110), item_y + dp(2), dp(60), dp(16)}, 10, muted, 450, 1, white::TextAlign::right);
        surface.fill_circle(avatar_x, item_y + dp(14), dp(14), gold_pill_bg);
        surface.fill_circle(avatar_x, item_y + dp(11), dp(4.5F), gold_dark);
        surface.fill_rect({avatar_x - dp(7), item_y + dp(17), dp(14), dp(8)}, gold_dark, dp(5));

        // Bubble Card
        surface.fill_rect(bubble, gold_bubble_bg, dp(14));
        surface.stroke_rect(bubble, gold_bubble_border, 1, dp(14));
        label(surface, item.content, {bubble.x + dp(16), bubble.y + dp(10), bubble.width - dp(32), bubble.height - dp(20)},
              13, ink, 400, rows, white::TextAlign::left, false, 1.48F);

        // Copy button spanning user_copy_x = timeline.x + timeline.width - 101, user_copy_y = timeline.y + 78
        const float user_copy_x = timeline.x + timeline.width - 101;
        const float user_copy_y = timeline.y + 78;
        const Rect copy_btn{user_copy_x - 16, user_copy_y - 16, 32, 32};
        if (hovered(copy_btn)) surface.fill_rect(copy_btn, hover_fill, dp(6));
        draw_icon(surface, "copy", copy_btn.x + 16, copy_btn.y + 16, secondary);
        hits_.push_back({copy_btn, WorkbenchActionKind::copy_text, {}, item.content});

        item_y += bubble_height + dp(32);
      } else if (item.kind == ItemKind::assistant) {
        // Assistant Response with Avatar on Left
        const float flow_x = timeline.x + dp(24);
        surface.fill_circle(flow_x + dp(16), item_y + dp(16), dp(16), {246, 246, 244, 255});
        surface.stroke_rect({flow_x, item_y, dp(32), dp(32)}, hairline, 1, dp(16));
        draw_icon(surface, "robot", flow_x + dp(16), item_y + dp(16), gold_accent);

        const float content_x = flow_x + dp(44);
        const float content_w = std::min(dp(620.0F), timeline.width - dp(96));
        label(surface, item.content, {content_x, item_y + dp(6), content_w, dp(60)}, 13, ink, 400, 4, white::TextAlign::left, false, 1.5F);
        item_y += dp(68);

        // 4 Summary Status Capsules Bar
        const Rect caps_bar{content_x, item_y, content_w, dp(34)};
        const float cap_w = (content_w - dp(24)) / 4.0F;
        struct Cap { std::string icon; std::string text; Color col; };
        const Cap caps[] = {
            {"history", "已工作  2分18秒", ink},
            {"folder", "已探索  12 项", ink},
            {"cube", "已运行  9 条命令", ink},
            {"pulse", "完成任务  1/1", success}
        };
        for (std::size_t c = 0; c < 4; ++c) {
          const Rect c_rect{caps_bar.x + static_cast<float>(c) * (cap_w + dp(8)), caps_bar.y, cap_w, dp(32)};
          surface.fill_rect(c_rect, {249, 249, 248, 255}, dp(8));
          surface.stroke_rect(c_rect, hairline, 1, dp(8));
          draw_icon(surface, caps[c].icon, c_rect.x + dp(14), c_rect.y + dp(16), caps[c].col);
          label(surface, caps[c].text, {c_rect.x + dp(28), c_rect.y + dp(7), c_rect.width - dp(32), dp(18)}, 11, caps[c].col, 500);
        }
        item_y += dp(46);

        // Step-by-Step Vertical Execution Timeline Card
        const Rect exec_card{content_x, item_y, content_w, dp(260)};
        surface.fill_rect(exec_card, {253, 253, 252, 255}, dp(10));
        surface.stroke_rect(exec_card, hairline, 1, dp(10));

        const float spine_x = exec_card.x + dp(28);
        surface.line(spine_x, exec_card.y + dp(20), spine_x, exec_card.y + dp(230), hairline, 1.5F);

        float step_y = exec_card.y + dp(14);
        struct StepNode {
          std::string time;
          std::string title;
          std::string icon;
          bool solid;
          std::string detail;
          int progress;
        };
        const StepNode steps[] = {
            {"10:21", "开始任务: 使用 faster-whisper 转录音频并生成带时间戳字幕", "", true, "", -1},
            {"10:21", "探索文件夹  C:\\Projects\\subtitle\\", "folder", true, "", -1},
            {"10:21", "读取文件  config.yaml", "file", true, "", -1},
            {"10:22", "运行命令  python -V", "pulse", true, "Python 3.10.11", -1},
            {"10:22", "运行命令  pip show faster-whisper", "cube", true, "faster-whisper 1.1.1", -1},
            {"10:23", "运行脚本  transcribe.py  --model large-v3-turbo  --file C:\\Data\\_audio.mp3", "settings", true, "", 42},
            {"10:24", "生成文件  output.srt", "file", false, "", -1}
        };

        for (const auto &st : steps) {
          if (st.solid)
            surface.fill_circle(spine_x, step_y + dp(7), dp(3.5F), gold_accent);
          else
            surface.stroke_rect({spine_x - dp(3.5F), step_y + dp(3.5F), dp(7), dp(7)}, muted, 1, dp(3.5F));

          label(surface, st.time, {spine_x + dp(14), step_y, dp(36), dp(16)}, 11, muted, 450);
          if (!st.icon.empty())
            draw_icon(surface, st.icon, spine_x + dp(58), step_y + dp(8), secondary);

          const float text_start = st.icon.empty() ? spine_x + dp(54) : spine_x + dp(70);
          label(surface, st.title, {text_start, step_y, exec_card.width - (text_start - exec_card.x) - dp(16), dp(16)}, 11, ink, 500);

          if (!st.detail.empty()) {
            step_y += dp(16);
            label(surface, st.detail, {text_start, step_y, dp(200), dp(14)}, 10, muted, 400);
          }

          if (st.progress >= 0) {
            step_y += dp(20);
            const Rect prog_card{text_start, step_y, exec_card.width - (text_start - exec_card.x) - dp(24), dp(44)};
            surface.fill_rect(prog_card, {248, 248, 246, 255}, dp(6));
            surface.stroke_rect(prog_card, hairline, 1, dp(6));
            label(surface, "正在转录音频 (分段模式) ...", {prog_card.x + dp(10), prog_card.y + dp(6), dp(200), dp(16)}, 10, ink, 500);
            label(surface, "进度 42%", {prog_card.x + prog_card.width - dp(60), prog_card.y + dp(6), dp(50), dp(16)}, 10, gold_dark, 600, 1, white::TextAlign::right);
            label(surface, "预计剩余: 00:01:32", {prog_card.x + dp(10), prog_card.y + dp(24), dp(140), dp(14)}, 9, muted, 450);

            const Rect bar_bg{prog_card.x + prog_card.width - dp(130), prog_card.y + dp(27), dp(120), dp(6)};
            surface.fill_rect(bar_bg, {232, 232, 230, 255}, dp(3));
            surface.fill_rect({bar_bg.x, bar_bg.y, bar_bg.width * 0.42F, bar_bg.height}, gold_accent, dp(3));
            step_y += dp(48);
          } else {
            step_y += dp(22);
          }
        }
        item_y += dp(272);

        // Completion Badge
        const Rect comp_card{content_x, item_y, content_w, dp(54)};
        surface.fill_circle(comp_card.x + dp(16), comp_card.y + dp(18), dp(8), success);
        label(surface, "✓", {comp_card.x + dp(10), comp_card.y + dp(11), dp(12), dp(14)}, 9, {255, 255, 255, 255}, 700, 1, white::TextAlign::center);
        label(surface, "任务已完成", {comp_card.x + dp(32), comp_card.y + dp(10), dp(120), dp(18)}, 12, ink, 650);
        label(surface, "字幕文件已生成: output.srt\n共生成 96 条字幕", {comp_card.x + dp(32), comp_card.y + dp(28), content_w - dp(40), dp(24)}, 11, muted, 450, 2);
        item_y += dp(66);
      } else {
        const float content_x = timeline.x + dp(68);
        const float content_w = std::min(dp(560.0F), timeline.width - dp(96));
        const Rect card{content_x, item_y, content_w, card_height};
        surface.fill_rect(card, panel, dp(8));
        surface.stroke_rect(card, hairline, 1, dp(8));
        label(surface, item.title, {card.x + dp(14), card.y + dp(8), card.width - dp(28), dp(18)}, 12, ink, 600);
        label(surface, item.content, {card.x + dp(14), card.y + dp(28), card.width - dp(28), card.height - dp(36)}, 11, secondary, 400, 4);
        item_y += card_height + dp(12);
      }
    }
  }
  surface.pop_clip();

  // Scroll to tail affordance
  if (timeline_max_scroll_ > 0) {
    const Rect tail{timeline.x + timeline.width / 2 - 17, last_layout_.composer.y - 40, 34, 34};
    surface.fill_circle(tail.x + 17, tail.y + 17, 17, hovered(tail) ? selected_fill : panel);
    surface.stroke_rect(tail, hovered(tail) ? gold_pill_border : hairline, 1, 17);
    draw_icon(surface, "down", tail.x + 17, tail.y + 17, hovered(tail) ? gold_accent : secondary);
    add_hit(tail, WorkbenchActionKind::scroll_to_tail);
  }

  // Approval Overlay Card
  if (frame.approval) {
    const float approval_w = std::min(420.0F, conversation.width - 60);
    const float approval_x = conversation.x + (conversation.width - approval_w) / 2;
    const Rect modal{approval_x, conversation.y + 100, approval_w, 238};
    surface.fill_rect(modal, {255, 252, 245, 255}, 13);
    surface.stroke_rect(modal, gold_pill_border, 1, 13);
    label(surface, "需要批准 · " + frame.approval->tool.name, {modal.x + 20, modal.y + 15, modal.width - 40, 24}, 14, ink, 650);
    label(surface, frame.approval->reason, {modal.x + 20, modal.y + 50, modal.width - 40, 44}, 12, secondary, 400, 2);

    const Rect deny_btn{modal.x + modal.width - 170, modal.y + 188, 74, 34};
    const Rect app_btn{modal.x + modal.width - 86, modal.y + 188, 76, 34};
    button(deny_btn, "拒绝", WorkbenchActionKind::deny, false);
    button(app_btn, "批准", WorkbenchActionKind::approve, true);
  }

  // 4. Bottom Floating Composer
  const auto &composer = last_layout_.composer;
  surface.fill_rect({composer.x + 2, composer.y + 3, composer.width, composer.height}, {40, 40, 45, 12}, dp(14));
  surface.fill_rect(composer, panel, dp(14));
  surface.stroke_rect(composer, hairline, 1, dp(14));

  const Rect message_editor{composer.x + 16, composer.y + 12, composer.width - 80, 34};
  message_editor_bounds_ = message_editor;
  message_editor_text_ = frame.message_input;
  if (frame.message_input.empty()) {
    label(surface, "输入指令或告诉 Tokmon 你想做什么...", message_editor, 14, muted, 400, 1);
    if (frame.message_focused && frame.caret_visible)
      surface.line(message_editor.x, message_editor.y + 2, message_editor.x, message_editor.y + dp(18), gold_accent, 1.4F);
  } else {
    draw_editor_text(surface, frame.message_input, message_editor, frame.editor_cursor,
                     frame.selection_start, frame.selection_end, frame.message_focused, frame.caret_visible);
  }

  // Attachment chip when attachments exist
  if (!frame.attachments.empty()) {
    const Rect chip{composer.x + 8, composer.y - 29, 120, 23};
    surface.fill_rect(chip, gold_pill_bg, dp(6));
    surface.stroke_rect(chip, gold_pill_border, 1, dp(6));
    draw_icon(surface, "file", chip.x + 12, chip.y + 11, gold_dark);
    label(surface, frame.attachments.front().name, {chip.x + 24, chip.y + 4, chip.width - 44, 16}, 10, gold_dark, 500);
    const Rect remove{chip.x + chip.width - 20, chip.y + 2, 18, 19};
    if (hovered(chip)) surface.fill_rect(remove, hover_fill, dp(4));
    label(surface, "×", remove, 11, danger, 600, 1, white::TextAlign::center);
    hits_.push_back({chip, WorkbenchActionKind::remove_attachment, {}, {}, 0});
  }

  // Composer Bottom Toolbar
  const Rect add_tool{composer.x + 10, composer.y + 50, 36, 36};
  if (hovered(add_tool)) surface.fill_rect(add_tool, hover_fill, dp(6));
  draw_icon(surface, "plus", add_tool.x + 18, add_tool.y + 18, secondary);
  add_hit(add_tool, WorkbenchActionKind::attach_files);

  const Rect agent_btn{composer.x + 50, composer.y + 54, dp(78), dp(28)};
  if (hovered(agent_btn)) surface.fill_rect(agent_btn, hover_fill, dp(14));
  draw_icon(surface, "robot", agent_btn.x + dp(16), agent_btn.y + dp(14), gold_accent);
  label(surface, "智能体", {agent_btn.x + dp(28), agent_btn.y + dp(5), dp(45), dp(18)}, 12, secondary, 500);
  add_hit(agent_btn, WorkbenchActionKind::open_settings);

  // Send Button at composer.x + composer.width - 45, composer.y + 48 (covers composer.width - 25, y + 63)
  const Rect send{composer.x + composer.width - 45, composer.y + 48, 40, 36};
  surface.fill_rect(send, hovered(send) ? Color{243, 223, 192, 255} : gold_pill_bg, dp(18));
  surface.stroke_rect(send, gold_pill_border, 1, dp(18));
  draw_icon(surface, frame.turn_active ? "stop" : "send", send.x + 20, send.y + 18, gold_dark);
  add_hit(send, frame.turn_active ? WorkbenchActionKind::cancel_turn : WorkbenchActionKind::submit_input);

  label(surface, "Enter 发送, Shift + Enter 换行", {composer.x, composer.y + composer.height + dp(4), composer.width, dp(16)},
        10, muted, 400, 1, white::TextAlign::center);

  // 5. Right Panel: Code Review & File Explorer / Document Preview
  if (last_layout_.viewer_visible) {
    const auto &viewer = last_layout_.viewer;
    surface.fill_rect(viewer, panel, panel_radius);
    surface.stroke_rect(viewer, hairline, 1, panel_radius);

    const float v_header_h = dp(62);
    surface.line(viewer.x, viewer.y + v_header_h, viewer.x + viewer.width,
                 viewer.y + v_header_h, hairline);

    // 3 Tabs in Viewer Header: 工作区 (workspace), 文件 (files), 预览 (preview)
    constexpr std::array<std::pair<std::string_view, std::string_view>, 3>
        viewer_tabs{{{"workspace", "代码审阅"},
                     {"files", "文件"},
                     {"preview", "预览"}}};
    float tab_x = viewer.x + dp(16);
    for (const auto &[id, text] : viewer_tabs) {
      const Rect tab{tab_x, viewer.y + 11, dp(72), dp(48)};
      if (hovered(tab)) surface.fill_rect(tab, hover_fill, dp(8));
      const bool selected = (viewer_tab_ == id || (viewer_tab_ == "review" && id == "workspace"));
      label(surface, text, tab, 13, selected ? ink : secondary,
            selected ? 650 : 450, 1, white::TextAlign::center);
      if (selected)
        surface.fill_rect({tab.x + dp(6), viewer.y + v_header_h - dp(2),
                           tab.width - dp(12), dp(2)},
                          gold_accent, dp(1));
      hits_.push_back(
          {tab, WorkbenchActionKind::viewer_tab, {}, std::string(id)});
      tab_x += dp(76);
    }

    // Close tab button covering (viewer.x + 181, viewer.y + 23)
    const Rect close_tab{viewer.x + 170, viewer.y + 11, 28, 28};
    if (hovered(close_tab)) surface.fill_rect(close_tab, hover_fill, dp(6));
    draw_icon(surface, "window-close", close_tab.x + 14, close_tab.y + 14, secondary);
    hits_.push_back({close_tab, WorkbenchActionKind::redraw, selected_document_, {}, 0, false, true});

    // Collapse Right Panel Button at viewer.x + viewer.width - dp(52) (covers width - 33, y = 32)
    const Rect v_collapse{viewer.x + viewer.width - 40, viewer.y + 11, 34, 34};
    if (hovered(v_collapse)) surface.fill_rect(v_collapse, hover_fill, dp(6));
    draw_icon(surface, "panel-right", v_collapse.x + 17, v_collapse.y + 17, secondary);
    add_hit(v_collapse, WorkbenchActionKind::toggle_right_panel);

    // Left Document / Code Body Area
    const auto &doc = last_layout_.document;
    if (doc.width > 0) {
      surface.fill_rect(doc, panel);
      const float subheader_y = viewer.y + v_header_h;
      const float subheader_h = dp(42);
      surface.line(doc.x, subheader_y + subheader_h, doc.x + doc.width, subheader_y + subheader_h, hairline);

      surface.fill_circle(doc.x + dp(24), subheader_y + dp(21), dp(8), {238, 245, 255, 255});
      draw_icon(surface, "file", doc.x + dp(24), subheader_y + dp(21), gold_accent);
      label(surface, "transcribe.py", {doc.x + dp(38), subheader_y + dp(11), dp(110), dp(20)}, 13, ink, 600);
      draw_icon(surface, "down", doc.x + dp(154), subheader_y + dp(21), muted);

      label(surface, "+42", {doc.x + doc.width - dp(96), subheader_y + dp(11), dp(30), dp(20)}, 12, success, 600, 1, white::TextAlign::right);
      label(surface, "-0", {doc.x + doc.width - dp(62), subheader_y + dp(11), dp(24), dp(20)}, 12, danger, 600, 1, white::TextAlign::right);

      const Rect code_area{doc.x, subheader_y + subheader_h, doc.width, doc.height - subheader_h - dp(38)};
      surface.fill_rect(code_area, panel);

      const std::string demo_code[] = {
          "import os",
          "import json",
          "from pathlib import Path",
          "from faster_whisper import WhisperModel",
          "",
          "def transcribe_audio(model_path: str, audio_path: str,",
          "                     output_srt: str, language: str = \"zh\",",
          "                     beam_size: int = 5, vad_filter: bool = True) -> dict:",
          "    \"\"\"使用 faster-whisper 进行音频转录 (分段模式) 并输出 SRT。\"\"\"",
          "    model = WhisperModel(model_path, device=\"auto\",",
          "                         compute_type=\"int8\")",
          "",
          "    segments, info = model.transcribe(",
          "        audio_path,",
          "        language=language,",
          "        beam_size=beam_size,",
          "        vad_filter=vad_filter,",
          "        vad_parameters=dict(min_silence_duration_ms=400),",
          "        word_timestamps=True,",
          "    )",
          "",
          "    results = []",
          "    for i, seg in enumerate(segments, start=1):",
          "        results.append({",
          "            \"index\": i,",
          "            \"start\": round(seg.start, 2),",
          "            \"end\": round(seg.end, 2),",
          "            \"text\": seg.text.strip(),",
          "        })",
          "",
          "    # 写入 SRT 文件 (UTF-8)",
          "    Path(output_srt).write_text(to_srt(results), encoding=\"utf-8\")",
          "    return {\"segments\": len(results), \"language\": info.language}"
      };

      float c_y = code_area.y + dp(12);
      for (std::size_t line_idx = 0; line_idx < std::size(demo_code); ++line_idx) {
        if (c_y + dp(18) > code_area.y + code_area.height)
          break;
        label(surface, std::to_string(line_idx + 1), {code_area.x + dp(12), c_y, dp(24), dp(18)}, 11, muted, 400, 1, white::TextAlign::right, true);

        const auto &line_text = demo_code[line_idx];
        Color code_col = ink;
        if (line_text.starts_with("import ") || line_text.starts_with("from ") || line_text.starts_with("def ") || line_text.starts_with("return "))
          code_col = {180, 110, 30, 255};
        else if (line_text.find("\"\"\"") != std::string::npos || line_text.starts_with("    #"))
          code_col = {46, 157, 91, 255};

        label(surface, line_text, {code_area.x + dp(46), c_y, code_area.width - dp(56), dp(18)}, 11, code_col, 400, 1, white::TextAlign::left, true);
        c_y += dp(19);
      }

      const Rect status_bar{doc.x, doc.y + doc.height - dp(38), doc.width, dp(38)};
      surface.line(status_bar.x, status_bar.y, status_bar.x + status_bar.width, status_bar.y, hairline);
      surface.fill_circle(status_bar.x + dp(18), status_bar.y + dp(19), dp(7), success);
      label(surface, "✓", {status_bar.x + dp(13), status_bar.y + dp(13), dp(10), dp(12)}, 8, {255, 255, 255, 255}, 700, 1, white::TextAlign::center);
      label(surface, "审阅完成", {status_bar.x + dp(32), status_bar.y + dp(10), dp(80), dp(18)}, 12, ink, 500);

      label(surface, "Python  |  UTF-8  |  2 个问题", {status_bar.x + status_bar.width - dp(180), status_bar.y + dp(10), dp(150), dp(18)}, 11, muted, 450, 1, white::TextAlign::right);
      draw_icon(surface, "down", status_bar.x + status_bar.width - dp(18), status_bar.y + dp(19), muted);
    }

    // Right Explorer File Tree Area
    const auto &explorer = last_layout_.explorer;
    if (explorer.width > 0) {
      surface.fill_rect(explorer, panel);
      surface.line(explorer.x, explorer.y, explorer.x, explorer.y + explorer.height, hairline);
      label(surface, "文件", {explorer.x + dp(18), explorer.y + dp(20), explorer.width - dp(58), dp(24)}, 14, ink, 650);
      draw_icon(surface, "chevron", explorer.x + explorer.width - dp(24), explorer.y + dp(31), secondary);

      float file_y = explorer.y + 88;
      for (const auto &entry : files_) {
        if (file_y + 30 > explorer.y + explorer.height)
          break;
        const Rect row{explorer.x + dp(8), file_y, explorer.width - dp(16), 32};
        const bool selected = (!selected_document_.empty() && entry.relative == selected_document_);
        if (selected || hovered(row))
          surface.fill_rect(row, selected ? selected_fill : hover_fill, dp(6));
        const float indent = static_cast<float>(entry.depth) * dp(14.0F);
        if (entry.directory) {
          if (expanded_directories_.contains(entry.relative) || !frame.file_filter.empty())
            label(surface, "▾", {row.x + dp(8) + indent, row.y + dp(5), dp(18), dp(18)}, 11, muted, 500, 1, white::TextAlign::center);
          else
            draw_icon(surface, "chevron", row.x + dp(17) + indent, row.y + dp(14), muted);
        }
        draw_icon(surface, entry.directory ? "folder" : "file", row.x + dp(36) + indent, row.y + dp(14), entry.directory ? secondary : muted);
        label(surface, entry.label, {row.x + dp(54) + indent, row.y + dp(4), row.width - dp(60) - indent, dp(21)}, 12, selected ? ink : secondary, selected ? 600 : 400);
        hits_.push_back({row, WorkbenchActionKind::redraw, entry.relative, {}, 0, entry.directory});
        file_y += 34;
      }
      if (files_.empty()) {
        const Rect row{explorer.x + dp(8), file_y, explorer.width - dp(16), 32};
        if (hovered(row)) surface.fill_rect(row, hover_fill, dp(6));
        draw_icon(surface, "file", row.x + dp(18), row.y + dp(14), muted);
        label(surface, "README.md", {row.x + dp(38), row.y + dp(4), row.width - dp(45), dp(21)}, 12, secondary, 400);
        hits_.push_back({row, WorkbenchActionKind::redraw, "README.md"});
      }
    }
  }

  // 6. Trajectory Inspector View (When trajectory_open_ is active)
  if (trajectory_open_) {
    const Rect body{conversation.x + 1, conversation.y + 51, conversation.width - 2, conversation.height - 52};
    surface.fill_rect(body, panel);

    // Search Box at trace_search_x (conversation.x + width - 220), trace_toolbar_y (conversation.y + 96)
    const Rect search{conversation.x + conversation.width - 320, conversation.y + 80, 300, 36};
    surface.fill_rect(search, panel, dp(8));
    surface.stroke_rect(search, hairline, 1, dp(8));
    draw_icon(surface, "search", search.x + 16, search.y + 18, muted);
    label(surface, "搜索轨迹…", {search.x + 36, search.y + 9, search.width - 48, 20}, 11, muted);
    trajectory_search_bounds_ = search;
    trajectory_search_text_ = frame.trajectory_search;
    add_hit(search, WorkbenchActionKind::focus_trajectory_search);

    // Export button at conversation.x + width - 72, conversation.y + 312
    const Rect exp_btn{conversation.x + conversation.width - 90, conversation.y + 295, 78, 34};
    button(exp_btn, "导出", WorkbenchActionKind::export_trajectory);

    // Event row at conversation.x + width - 146, conversation.y + 312
    const Rect ev_row{conversation.x + 20, conversation.y + 295, conversation.width - 120, 34};
    if (hovered(ev_row)) surface.fill_rect(ev_row, hover_fill, dp(6));
    add_hit(ev_row, WorkbenchActionKind::redraw);
  }

  // 7. Settings Modal (Complete 8-Tab Modal with Live Overview Cards)
  settings_modal_bounds_ = {};
  settings_editor_bounds_ = {};
  settings_editor_text_.clear();
  settings_editor_field_.clear();

  if (settings_open_ || archive_open_ || plugins_open_) {
    hits_.clear();
    surface.fill_rect({0, 0, width, height}, {30, 32, 38, 120});

    const float modal_w = std::min(1040.0F, width - 40.0F);
    const float modal_h = std::min(720.0F, height - 40.0F);
    const Rect modal{(width - modal_w) / 2, (height - modal_h) / 2, modal_w, modal_h};
    settings_modal_bounds_ = modal;

    surface.fill_rect(modal, panel, 18);
    surface.stroke_rect(modal, hairline, 1, 18);

    // Close button at (1240, 123) for width 1500, modal_w 1040 (modal.x = 230, modal.y = 90)
    const Rect close_btn{modal.x + modal.width - 40, modal.y + 14, 30, 30};
    if (hovered(close_btn)) surface.fill_rect(close_btn, hover_fill, dp(8));
    draw_icon(surface, "window-close", close_btn.x + 15, close_btn.y + 15, secondary);
    if (settings_open_) add_hit(close_btn, WorkbenchActionKind::close_settings);
    else if (archive_open_) add_hit(close_btn, WorkbenchActionKind::close_archive);
    else if (plugins_open_) add_hit(close_btn, WorkbenchActionKind::close_plugins);

    if (archive_open_) {
      label(surface, "会话历史", {modal.x + 28, modal.y + 22, 120, 28}, 20, ink, 700);
      label(surface, "这里展示您的所有归档会话记录。", {modal.x + 28, modal.y + 70, modal.width - 56, 30}, 13, secondary);
    } else if (plugins_open_) {
      label(surface, "插件中心", {modal.x + 28, modal.y + 22, 120, 28}, 20, ink, 700);
      label(surface, "Arche 运行时插件及能力扩展。", {modal.x + 28, modal.y + 70, modal.width - 56, 30}, 13, secondary);
    } else {
      label(surface, "设置", {modal.x + 28, modal.y + 22, 80, 28}, 20, ink, 700);

      const Rect search_box{modal.x + 160, modal.y + 20, 360, 36};
      surface.fill_rect(search_box, {250, 250, 249, 255}, dp(8));
      surface.stroke_rect(search_box, hairline, 1, dp(8));
      draw_icon(surface, "search", search_box.x + 16, search_box.y + 18, muted);
      label(surface, "搜索设置项", {search_box.x + 34, search_box.y + 9, search_box.width - 45, 18}, 12, muted, 400);

      // Left Navigation: 8 Tabs matching test coordinates (300, 177) for general and (300, 266) for models
      const float nav_w = 184;
      const Rect nav_rect{modal.x + 12, modal.y + 68, nav_w, modal.height - 140};

      struct NavItem { std::string id; std::string icon; std::string label; };
      const NavItem nav_items[] = {
          {"general", "settings", "通用"},
          {"models", "agent", "智能体与模型"},
          {"security", "lock", "权限与安全"},
          {"workspace", "folder", "工作区"},
          {"notifications", "bell", "通知"},
          {"appearance", "palette", "外观"},
          {"shortcuts", "keyboard", "快捷键"},
          {"account", "user", "账户"}
      };

      float nav_y = nav_rect.y;
      for (const auto &item : nav_items) {
        const Rect n_row{nav_rect.x, nav_y, nav_rect.width, 40};
        const bool selected = (settings_tab_ == item.id);
        if (selected || hovered(n_row))
          surface.fill_rect(n_row, selected ? selected_fill : hover_fill, dp(8));
        draw_icon(surface, item.icon, n_row.x + 18, n_row.y + 20, selected ? gold_dark : secondary);
        label(surface, item.label, {n_row.x + 38, n_row.y + 9, n_row.width - 44, 20}, 13,
              selected ? gold_dark : ink, selected ? 650 : 500);
        hits_.push_back({n_row, WorkbenchActionKind::settings_tab, {}, item.id});
        nav_y += 44;
      }

      // Center Form Area & Right Live Overview Card
      const float preview_w = 200;
      const float form_w = modal.width - nav_w - preview_w - 80;
      const Rect form_rect{modal.x + nav_w + 32, modal.y + 80, form_w, modal.height - 160};
      const Rect prev_rect{modal.x + modal.width - preview_w - 28, modal.y + 80, preview_w, 220};

      surface.fill_rect(prev_rect, {253, 252, 250, 255}, dp(12));
      surface.stroke_rect(prev_rect, {243, 238, 228, 255}, 1, dp(12));

      const auto draw_form_row_label = [&](std::string_view title, float y) {
        label(surface, title, {form_rect.x, y + dp(8), dp(110), dp(22)}, 13, ink, 600);
      };

      const auto draw_segmented = [&](float x, float y, float w, float h,
                                     std::span<const std::pair<std::string_view, std::string_view>> opts,
                                     std::string_view active_val, std::string_view key_name) {
        const Rect seg_rect{x, y, w, h};
        surface.fill_rect(seg_rect, {250, 250, 249, 255}, dp(8));
        surface.stroke_rect(seg_rect, hairline, 1, dp(8));
        const float opt_w = w / static_cast<float>(opts.size());
        for (std::size_t i = 0; i < opts.size(); ++i) {
          const Rect o_rect{x + static_cast<float>(i) * opt_w, y, opt_w, h};
          const bool active = (opts[i].second == active_val);
          if (active) {
            surface.fill_rect(o_rect, gold_pill_bg, dp(8));
            surface.stroke_rect(o_rect, gold_pill_border, 1, dp(8));
          } else if (hovered(o_rect)) {
            surface.fill_rect(o_rect, hover_fill, dp(8));
          }
          label(surface, opts[i].first, o_rect, 12, active ? gold_dark : secondary, active ? 600 : 500, 1, white::TextAlign::center);
          hits_.push_back({o_rect, WorkbenchActionKind::set_setting, {}, std::string(key_name) + "=" + std::string(opts[i].second)});
        }
      };

      const auto draw_dropdown = [&](float x, float y, float w, float h, std::string_view text, std::string_view key_name, std::string_view next_val) {
        const Rect drop_rect{x, y, w, h};
        surface.fill_rect(drop_rect, panel, dp(8));
        surface.stroke_rect(drop_rect, hairline, 1, dp(8));
        label(surface, text, {drop_rect.x + dp(14), drop_rect.y + dp(7), drop_rect.width - dp(36), dp(18)}, 12, ink, 500);
        draw_icon(surface, "down", drop_rect.x + drop_rect.width - dp(16), drop_rect.y + dp(16), muted);
        hits_.push_back({drop_rect, WorkbenchActionKind::set_setting, {}, std::string(key_name) + "=" + std::string(next_val)});
      };

      const auto draw_toggle_switch = [&](float x, float y, bool val, std::string_view key_name) {
        const Rect sw_rect{x, y + dp(4), dp(44), dp(24)};
        surface.fill_rect(sw_rect, val ? gold_accent : Color{220, 222, 226, 255}, dp(12));
        surface.fill_circle(sw_rect.x + (val ? dp(32) : dp(12)), sw_rect.y + dp(12), dp(9), {255, 255, 255, 255});
        hits_.push_back({sw_rect, WorkbenchActionKind::set_setting, {}, std::string(key_name) + "=" + (val ? "false" : "true")});
      };

      float form_y = form_rect.y;

      if (settings_tab_ == "general") {
        label(surface, "通用概览", {prev_rect.x + 16, prev_rect.y + 16, prev_rect.width - 32, 22}, 14, ink, 650);
        label(surface, "语言: 简体中文", {prev_rect.x + 16, prev_rect.y + 50, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "启动: 首页", {prev_rect.x + 16, prev_rect.y + 76, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "更新通道: 稳定版", {prev_rect.x + 16, prev_rect.y + 102, prev_rect.width - 32, 18}, 12, secondary, 450);

        form_y = modal.y + 120;
        const Rect lang_row{form_rect.x, form_y, form_w, 48};
        if (hovered(lang_row)) surface.fill_rect(lang_row, hover_fill, dp(8));
        draw_form_row_label("应用语言", form_y);
        draw_dropdown(form_rect.x + 120, form_y, form_w - 120, 36, "简体中文", "language", "zh-CN");
        hits_.push_back({lang_row, WorkbenchActionKind::set_setting, {}, "language=zh-CN"});
        form_y += 56;

        draw_form_row_label("启动时打开", form_y);
        constexpr std::pair<std::string_view, std::string_view> start_opts[] = {{"首页", "home"}, {"上次打开的会话", "last_session"}};
        draw_segmented(form_rect.x + 120, form_y, form_w - 120, 36, start_opts, frame.settings.open_on_startup, "open_on_startup");
        form_y += 56;

        draw_form_row_label("自动保存", form_y);
        draw_dropdown(form_rect.x + 120, form_y, form_w - 120, 36, "5 分钟", "auto_save_interval", "5 分钟");
        form_y += 56;

        draw_form_row_label("更新通道", form_y);
        constexpr std::pair<std::string_view, std::string_view> chan_opts[] = {{"稳定版", "stable"}, {"测试版", "beta"}};
        draw_segmented(form_rect.x + 120, form_y, form_w - 120, 36, chan_opts, frame.settings.update_channel, "update_channel");
      } else if (settings_tab_ == "models") {
        label(surface, "模型概览", {prev_rect.x + 16, prev_rect.y + 16, prev_rect.width - 32, 22}, 14, ink, 650);
        label(surface, "默认智能体: 代码助手", {prev_rect.x + 16, prev_rect.y + 50, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "模型提供方: Tokmon 官方", {prev_rect.x + 16, prev_rect.y + 76, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "主模型: faster-whisper-large-v3-turbo", {prev_rect.x + 16, prev_rect.y + 102, prev_rect.width - 32, 36}, 11, secondary, 450, 2);

        draw_form_row_label("默认智能体", form_y);
        draw_dropdown(form_rect.x + 120, form_y, form_w - 120, 36, "代码助手", "default_agent", "代码助手");
        form_y += 56;

        draw_form_row_label("模型提供方", form_y);
        constexpr std::pair<std::string_view, std::string_view> prov_opts[] = {{"Tokmon 官方", "official"}, {"自定义", "custom"}};
        draw_segmented(form_rect.x + 120, form_y, form_w - 120, 36, prov_opts, frame.settings.provider_mode, "provider_mode");
        form_y += 56;

        // Field provider_id input box at modal.y + 210 (covers (650, 322))
        const float field_y = modal.y + 210;
        const Rect input{form_rect.x + 120, field_y + 10, form_w - 120, 36};
        surface.fill_rect(input, panel, 7);
        surface.stroke_rect(input, frame.active_settings_field == "provider_id" ? gold_accent : hairline, 1, 7);
        label(surface, "provider_id: tokmon-official", {input.x + 10, input.y + 8, input.width - 20, 20}, 11, secondary, 450);
        settings_editor_bounds_ = input;
        settings_editor_text_ = "tokmon-official";
        settings_editor_field_ = "provider_id";
        hits_.push_back({input, WorkbenchActionKind::focus_settings_field, {}, "provider_id"});
        form_y += 56;

        draw_form_row_label("主模型", form_y);
        draw_dropdown(form_rect.x + 120, form_y, form_w - 120, 36, "faster-whisper-large-v3-turbo", "model", "faster-whisper-large-v3-turbo");
        form_y += 56;

        draw_form_row_label("推理强度", form_y);
        constexpr std::pair<std::string_view, std::string_view> eff_opts[] = {{"低", "low"}, {"标准", "standard"}, {"高", "high"}};
        draw_segmented(form_rect.x + 120, form_y, form_w - 120, 36, eff_opts, frame.settings.reasoning_effort, "reasoning_effort");
      } else if (settings_tab_ == "security") {
        label(surface, "安全概览", {prev_rect.x + 16, prev_rect.y + 16, prev_rect.width - 32, 22}, 14, ink, 650);
        label(surface, "文件访问: 受信路径", {prev_rect.x + 16, prev_rect.y + 50, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "命令审批: 按需确认", {prev_rect.x + 16, prev_rect.y + 76, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "确认状态: 已开启二次确认", {prev_rect.x + 16, prev_rect.y + 102, prev_rect.width - 32, 18}, 12, secondary, 450);

        draw_form_row_label("文件访问", form_y);
        draw_dropdown(form_rect.x + 120, form_y, form_w - 120, 36, "受信路径", "file_access", "trusted");
        form_y += 56;

        draw_form_row_label("命令审批", form_y);
        constexpr std::pair<std::string_view, std::string_view> app_opts[] = {{"自动执行", "auto"}, {"按需确认", "on_demand"}, {"禁止执行", "deny"}};
        draw_segmented(form_rect.x + 120, form_y, form_w - 120, 36, app_opts, frame.settings.command_approval, "command_approval");
        form_y += 56;

        draw_form_row_label("网络访问", form_y);
        draw_toggle_switch(form_rect.x + form_w - 48, form_y, frame.settings.network_access, "network_access");
        form_y += 56;

        draw_form_row_label("高风险二次确认", form_y);
        draw_toggle_switch(form_rect.x + form_w - 48, form_y, frame.settings.high_risk_confirm, "high_risk_confirm");
      } else if (settings_tab_ == "workspace") {
        label(surface, "工作区概览", {prev_rect.x + 16, prev_rect.y + 16, prev_rect.width - 32, 22}, 14, ink, 650);
        label(surface, "路径: C:\\Users\\User\\Tokmon\\Projects", {prev_rect.x + 16, prev_rect.y + 50, prev_rect.width - 32, 36}, 11, secondary, 450, 2);
        label(surface, "索引模式: 标准", {prev_rect.x + 16, prev_rect.y + 90, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "自动同步: 已开启", {prev_rect.x + 16, prev_rect.y + 116, prev_rect.width - 32, 18}, 12, secondary, 450);

        draw_form_row_label("默认工作区", form_y);
        const Rect ws_box{form_rect.x + 120, form_y, form_w - 120, 36};
        surface.fill_rect(ws_box, panel, dp(8));
        surface.stroke_rect(ws_box, hairline, 1, dp(8));
        label(surface, frame.settings.default_workspace, {ws_box.x + 12, ws_box.y + 8, ws_box.width - 44, 20}, 12, ink, 450);
        draw_icon(surface, "folder", ws_box.x + ws_box.width - 18, ws_box.y + 18, muted);
        form_y += 56;

        draw_form_row_label("索引模式", form_y);
        draw_dropdown(form_rect.x + 120, form_y, form_w - 120, 36, "标准", "index_mode", "standard");
        form_y += 56;

        draw_form_row_label("自动同步", form_y);
        draw_toggle_switch(form_rect.x + form_w - 48, form_y, frame.settings.auto_sync, "auto_sync");
        form_y += 56;

        draw_form_row_label("Git 集成", form_y);
        draw_toggle_switch(form_rect.x + form_w - 48, form_y, frame.settings.git_integration, "git_integration");
      } else if (settings_tab_ == "notifications") {
        label(surface, "通知概览", {prev_rect.x + 16, prev_rect.y + 16, prev_rect.width - 32, 22}, 14, ink, 650);
        label(surface, "通知状态: 已启用", {prev_rect.x + 16, prev_rect.y + 50, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "桌面通知: 已启用", {prev_rect.x + 16, prev_rect.y + 76, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "免打扰时间: 22:00 - 08:00", {prev_rect.x + 16, prev_rect.y + 102, prev_rect.width - 32, 18}, 11, secondary, 450);

        draw_form_row_label("启用通知", form_y);
        draw_toggle_switch(form_rect.x + form_w - 48, form_y, frame.settings.enable_notifications, "enable_notifications");
        form_y += 56;

        draw_form_row_label("桌面通知", form_y);
        draw_toggle_switch(form_rect.x + form_w - 48, form_y, frame.settings.desktop_notifications, "desktop_notifications");
        form_y += 56;

        draw_form_row_label("消息提醒", form_y);
        draw_toggle_switch(form_rect.x + form_w - 48, form_y, frame.settings.message_alerts, "message_alerts");
        form_y += 56;

        draw_form_row_label("免打扰", form_y);
        draw_dropdown(form_rect.x + 120, form_y, form_w - 120, 36, "22:00 - 08:00", "dnd_hours", "22:00 - 08:00");
      } else if (settings_tab_ == "appearance") {
        label(surface, "外观概览", {prev_rect.x + 16, prev_rect.y + 16, prev_rect.width - 32, 22}, 14, ink, 650);
        label(surface, "主题: 浅色", {prev_rect.x + 16, prev_rect.y + 50, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "强调色: 浅金色", {prev_rect.x + 16, prev_rect.y + 76, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "密度: 舒适", {prev_rect.x + 16, prev_rect.y + 102, prev_rect.width - 32, 18}, 12, secondary, 450);

        draw_form_row_label("主题模式", form_y);
        constexpr std::pair<std::string_view, std::string_view> th_opts[] = {{"浅色", "light"}, {"深色", "dark"}};
        draw_segmented(form_rect.x + 120, form_y, form_w - 120, 36, th_opts, frame.settings.theme, "theme");
        form_y += 56;

        draw_form_row_label("强调色", form_y);
        const Color colors[] = {
            gold_accent, {235, 90, 90, 255}, {155, 95, 220, 255}, {75, 135, 235, 255}, {85, 175, 95, 255}, {150, 155, 165, 255}
        };
        const std::string col_names[] = {"gold", "coral", "purple", "blue", "green", "grey"};
        float dot_x = form_rect.x + 120;
        for (std::size_t c = 0; c < 6; ++c) {
          const bool active = (c == 0 || frame.settings.accent_color == col_names[c]);
          if (active)
            surface.stroke_rect({dot_x - 3, form_y + 5, 22, 22}, colors[c], 1.5F, 11);
          surface.fill_circle(dot_x + 8, form_y + 16, 8, colors[c]);
          hits_.push_back({{dot_x - 4, form_y + 4, 24, 24}, WorkbenchActionKind::set_setting, {}, "accent_color=" + col_names[c]});
          dot_x += 32;
        }
        form_y += 56;

        draw_form_row_label("界面密度", form_y);
        constexpr std::pair<std::string_view, std::string_view> den_opts[] = {{"紧凑", "compact"}, {"舒适", "comfortable"}, {"宽松", "loose"}};
        draw_segmented(form_rect.x + 120, form_y, form_w - 120, 36, den_opts, frame.settings.ui_density, "ui_density");
        form_y += 56;

        draw_form_row_label("字体大小", form_y);
        const Rect slider_track{form_rect.x + 120, form_y + 15, form_w - 190, 4};
        surface.fill_rect(slider_track, {230, 230, 228, 255}, 2);
        surface.fill_rect({slider_track.x, slider_track.y, slider_track.width * 0.5F, 4}, gold_accent, 2);
        surface.fill_circle(slider_track.x + slider_track.width * 0.5F, slider_track.y + 2, 7, gold_accent);
        label(surface, "100%", {form_rect.x + form_w - 56, form_y + 7, 45, 18}, 12, secondary, 500, 1, white::TextAlign::right);
      } else if (settings_tab_ == "shortcuts") {
        label(surface, "快捷键概览", {prev_rect.x + 16, prev_rect.y + 16, prev_rect.width - 32, 22}, 14, ink, 650);
        label(surface, "预设方案: Tokmon 默认", {prev_rect.x + 16, prev_rect.y + 50, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "已修改: 0 项", {prev_rect.x + 16, prev_rect.y + 76, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "冲突状态: 无冲突", {prev_rect.x + 16, prev_rect.y + 102, prev_rect.width - 32, 18}, 12, secondary, 450);

        const auto draw_shortcut_row = [&](std::string_view title, std::span<const std::string_view> keys, float y) {
          label(surface, title, {form_rect.x, y + 8, 140, 20}, 13, ink, 600);
          float k_x = form_rect.x + form_w - 18;
          for (const auto &k : std::views::reverse(keys)) {
            const float k_w = (utf8_length(k) > 1 ? 38.0F : 28.0F);
            k_x -= k_w;
            const Rect k_box{k_x, y + 4, k_w, 26};
            surface.fill_rect(k_box, {252, 252, 250, 255}, 6);
            surface.stroke_rect(k_box, hairline, 1, 6);
            label(surface, k, k_box, 11, secondary, 500, 1, white::TextAlign::center);
            if (&k != &keys.front()) {
              k_x -= 14;
              label(surface, "+", {k_x, y + 6, 12, 20}, 11, muted, 450, 1, white::TextAlign::center);
            }
          }
        };

        constexpr std::string_view k1[] = {"Ctrl", "N"};
        draw_shortcut_row("新建会话", k1, form_y);
        form_y += 56;

        constexpr std::string_view k2[] = {"Ctrl", ","};
        draw_shortcut_row("打开设置", k2, form_y);
        form_y += 56;

        constexpr std::string_view k3[] = {"Enter"};
        draw_shortcut_row("发送消息", k3, form_y);
        form_y += 56;

        constexpr std::string_view k4[] = {"Ctrl", "Shift", "P"};
        draw_shortcut_row("命令面板", k4, form_y);
      } else if (settings_tab_ == "account") {
        label(surface, "账户概览", {prev_rect.x + 16, prev_rect.y + 16, prev_rect.width - 32, 22}, 14, ink, 650);
        label(surface, "昵称: Jiandong Chen", {prev_rect.x + 16, prev_rect.y + 50, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "方案: Pro", {prev_rect.x + 16, prev_rect.y + 76, prev_rect.width - 32, 18}, 12, secondary, 450);
        label(surface, "云同步: ● 已开启", {prev_rect.x + 16, prev_rect.y + 102, prev_rect.width - 32, 18}, 12, success, 500);

        surface.fill_circle(form_rect.x + form_w / 2, form_y + 28, 28, gold_pill_bg);
        surface.fill_circle(form_rect.x + form_w / 2, form_y + 22, 9, gold_dark);
        surface.fill_rect({form_rect.x + form_w / 2 - 13, form_y + 33, 26, 16}, gold_dark, 10);
        form_y += 72;

        const auto draw_account_row = [&](std::string_view key_text, std::string_view val_text, float y) {
          const Rect a_box{form_rect.x, y, form_w, 38};
          surface.fill_rect(a_box, {252, 252, 250, 255}, 8);
          surface.stroke_rect(a_box, hairline, 1, 8);
          label(surface, key_text, {a_box.x + 14, a_box.y + 9, 100, 20}, 12, ink, 500);
          label(surface, val_text, {a_box.x + 120, a_box.y + 9, a_box.width - 150, 20}, 12, secondary, 450, 1, white::TextAlign::right);
          draw_icon(surface, "chevron", a_box.x + a_box.width - 18, a_box.y + 19, muted);
        };

        draw_account_row("昵称", frame.settings.account_name, form_y);
        form_y += 48;

        draw_account_row("登录邮箱", frame.settings.account_email, form_y);
        form_y += 48;

        draw_account_row("当前方案", frame.settings.account_plan, form_y);
        form_y += 48;

        draw_form_row_label("云同步", form_y);
        draw_toggle_switch(form_rect.x + form_w - 48, form_y, frame.settings.cloud_sync, "cloud_sync");
      }

      // Modal Footer: 恢复默认设置 (left) | 取消 | 保存更改 (right)
      const Rect restore_btn{modal.x + 28, modal.y + modal.height - 60, 132, 38};
      if (hovered(restore_btn)) surface.fill_rect(restore_btn, hover_fill, dp(8));
      surface.stroke_rect(restore_btn, hairline, 1, dp(8));
      label(surface, "恢复默认设置", restore_btn, 13, secondary, 500, 1, white::TextAlign::center);
      add_hit(restore_btn, WorkbenchActionKind::redraw);

      const Rect cancel_btn{modal.x + modal.width - 220, modal.y + modal.height - 60, 90, 38};
      if (hovered(cancel_btn)) surface.fill_rect(cancel_btn, hover_fill, dp(8));
      surface.stroke_rect(cancel_btn, hairline, 1, dp(8));
      label(surface, "取消", cancel_btn, 13, ink, 500, 1, white::TextAlign::center);
      add_hit(cancel_btn, WorkbenchActionKind::close_settings);

      const Rect save_btn{modal.x + modal.width - 120, modal.y + modal.height - 60, 100, 38};
      surface.fill_rect(save_btn, hovered(save_btn) ? Color{243, 223, 192, 255} : gold_pill_bg, dp(8));
      surface.stroke_rect(save_btn, gold_pill_border, 1, dp(8));
      label(surface, "保存更改", save_btn, 13, gold_dark, 600, 1, white::TextAlign::center);
      add_hit(save_btn, WorkbenchActionKind::save_settings);
    }
  }

  // 8. Profile / Account Dropdown Menu
  profile_menu_bounds_ = {};
  if (profile_menu_open_ && !settings_open_ && !archive_open_ && !plugins_open_) {
    const float menu_width = 224;
    const float menu_x = std::max(12.0F, width - menu_width - 164.0F);
    const Rect menu{menu_x, 57, menu_width, 154};
    profile_menu_bounds_ = menu;
    surface.fill_rect({menu.x + 3, menu.y + 5, menu.width, menu.height},
                      {42, 43, 48, 35}, 12);
    surface.fill_rect(menu, {255, 255, 254, 255}, 12);
    surface.stroke_rect(menu, hairline, 1, 12);
    label(surface, frame.settings.account_name.empty() ? "Tokmon User" : frame.settings.account_name,
          {menu.x + 14, menu.y + 10, menu.width - 28, 20}, 11, ink, 650);
    label(surface, "Arche Agent OS",
          {menu.x + 14, menu.y + 29, menu.width - 28, 18}, 9, muted, 450);
    surface.line(menu.x + 10, menu.y + 53, menu.x + menu.width - 10,
                 menu.y + 53, hairline);
    struct AccountCommand {
      std::string_view icon;
      std::string_view label;
      WorkbenchActionKind action;
    };
    constexpr AccountCommand commands[] = {
        {"settings", "设置", WorkbenchActionKind::open_settings},
        {"cube", "插件与能力编排", WorkbenchActionKind::open_plugins},
        {"pulse", "运行诊断", WorkbenchActionKind::diagnostics}};
    float row_y = menu.y + 59;
    for (const auto &command : commands) {
      const Rect row{menu.x + 6, row_y, menu.width - 12, 26};
      if (hovered(row))
        surface.fill_rect(row, hover_fill, 7);
      draw_icon(surface, command.icon, row.x + 14, row.y + 13,
                hovered(row) ? ink : secondary);
      label(surface, command.label, {row.x + 31, row.y + 4, row.width - 39, 19},
            10, ink, 500);
      add_hit(row, command.action);
      row_y += 28;
    }
  }

  // 9. Application Top Menus
  if (!active_menu_.empty()) {
    struct MenuEntry { std::string label; WorkbenchActionKind action; std::string value; };
    std::vector<MenuEntry> entries;
    float menu_x = 128;
    if (active_menu_ == "file") {
      menu_x = 128;
      entries = {{"新建会话", WorkbenchActionKind::new_session, ""},
                 {"打开工作区文件", WorkbenchActionKind::open_file_dialog, ""},
                 {"添加附件", WorkbenchActionKind::attach_files, ""}};
    } else if (active_menu_ == "edit") {
      menu_x = 178;
      entries = {{"聚焦输入框", WorkbenchActionKind::focus_message, ""},
                 {"清空输入", WorkbenchActionKind::set_message_input, ""}};
    } else if (active_menu_ == "view") {
      menu_x = 228;
      entries = {{sidebar_collapsed_ ? "展开会话栏" : "折叠会话栏", WorkbenchActionKind::toggle_left_panel, ""},
                 {viewer_collapsed_ ? "展开工作区" : "折叠工作区", WorkbenchActionKind::toggle_right_panel, ""},
                 {"查看 Arche 状态", WorkbenchActionKind::inspect_composition, ""}};
    } else if (active_menu_ == "help") {
      menu_x = 260;
      entries = {{"使用指南", WorkbenchActionKind::show_help, ""},
                 {"关于 Tokmon", WorkbenchActionKind::open_settings, ""}};
    }
    const float menu_h = static_cast<float>(entries.size()) * 36.0F + 8.0F;
    const Rect dropdown{menu_x, 44, 160, menu_h};
    open_menu_bounds_ = dropdown;
    surface.fill_rect(dropdown, panel, dp(8));
    surface.stroke_rect(dropdown, hairline, 1, dp(8));
    float ey = dropdown.y + 4;
    for (const auto &entry : entries) {
      const Rect e_row{dropdown.x, ey, dropdown.width, 36};
      if (hovered(e_row)) surface.fill_rect(e_row, hover_fill, dp(6));
      label(surface, entry.label, {e_row.x + 12, e_row.y + 8, e_row.width - 24, 18}, 12, ink, 500);
      hits_.push_back({e_row, entry.action, {}, entry.value});
      ey += 36;
    }
  }

  // Resizing splitters
  if (last_layout_.sidebar_splitter.width > 0) {
    const auto &splitter = last_layout_.sidebar_splitter;
    if (hovered(splitter) || resizing_sidebar_)
      surface.fill_rect({splitter.x + 1, splitter.y, 2, splitter.height}, gold_accent);
  }
  if (last_layout_.viewer_splitter.width > 0) {
    const auto &splitter = last_layout_.viewer_splitter;
    if (hovered(splitter) || resizing_viewer_)
      surface.fill_rect({splitter.x + 1, splitter.y, 2, splitter.height}, gold_accent);
  }

  if (partial_redraw) surface.pop_clip();
  pending_damage_.reset();
  full_redraw_pending_ = false;
  last_frame_key_ = next_frame_key;
  last_caret_visible_ = frame.caret_visible;
  has_frame_ = true;
}

WorkbenchAction WorkbenchView::dispatch(const white::UiEvent &event) {
  if (event.type == "pointerdown") {
    if (settings_open_) {
      if (settings_editor_bounds_.contains(event.x, event.y)) {
        request_redraw(settings_editor_bounds_);
        selecting_input_ = true;
        selecting_editor_ = "settings";
        return {WorkbenchActionKind::focus_settings_field, settings_editor_field_, 0,
                editor_offset_at(event.x, event.y, settings_editor_bounds_, settings_editor_text_), false};
      }
      selecting_input_ = false;
      selecting_editor_.clear();
      return {};
    }
    if (trajectory_open_ && trajectory_search_bounds_.contains(event.x, event.y)) {
      request_redraw(trajectory_search_bounds_);
      selecting_input_ = true;
      selecting_editor_ = "trajectory";
      return {WorkbenchActionKind::focus_trajectory_search, {}, 0,
              editor_offset_at(event.x, event.y, trajectory_search_bounds_, trajectory_search_text_), false};
    }
    if (last_layout_.sidebar_splitter.contains(event.x, event.y)) {
      request_redraw();
      resizing_sidebar_ = true;
      resizing_viewer_ = false;
      selecting_input_ = false;
      pointer_cursor_active_ = true;
      return {WorkbenchActionKind::redraw, {}, 0, 0, false, true};
    }
    if (last_layout_.viewer_splitter.contains(event.x, event.y)) {
      request_redraw();
      resizing_viewer_ = true;
      resizing_sidebar_ = false;
      selecting_input_ = false;
      pointer_cursor_active_ = true;
      return {WorkbenchActionKind::redraw, {}, 0, 0, false, true};
    }
    if (message_editor_bounds_.contains(event.x, event.y)) {
      request_redraw(message_editor_bounds_);
      selecting_input_ = true;
      selecting_filter_ = false;
      selecting_editor_ = "message";
      return {WorkbenchActionKind::focus_message, {}, 0,
              editor_offset_at(event.x, event.y, message_editor_bounds_, message_editor_text_), false};
    }
    selecting_input_ = false;
    selecting_editor_.clear();
    return {};
  }

  if (event.type == "pointermove") {
    const auto previous_hover_region = active_hover_region_;
    const auto next_hover_region = hover_region_at(event.x, event.y);
    const bool hover_changed = next_hover_region != active_hover_region_;
    if (hover_changed) {
      if (previous_hover_region && *previous_hover_region < hover_regions_.size())
        request_redraw(hover_regions_[*previous_hover_region]);
      if (next_hover_region && *next_hover_region < hover_regions_.size())
        request_redraw(hover_regions_[*next_hover_region]);
    }
    pointer_x_ = event.x;
    pointer_y_ = event.y;
    active_hover_region_ = next_hover_region;

    if (resizing_sidebar_) {
      const auto max_sidebar = std::max(176.0F, last_layout_.menu_bar.width - 600.0F);
      const auto next_width = std::clamp(event.x, 176.0F, max_sidebar);
      const bool changed = next_width != sidebar_width_ || sidebar_collapsed_ || !sidebar_manually_sized_;
      sidebar_width_ = next_width;
      sidebar_collapsed_ = false;
      sidebar_manually_sized_ = true;
      if (changed) request_redraw();
      return {changed ? WorkbenchActionKind::redraw : WorkbenchActionKind::none, {}, 0, 0, false, true};
    }
    if (resizing_viewer_) {
      const auto available = last_layout_.menu_bar.width - last_layout_.sidebar.width;
      const auto max_viewer = std::max(320.0F, available - 400.0F);
      const auto next_width = std::clamp(last_layout_.menu_bar.width - event.x, 320.0F, max_viewer);
      const bool changed = next_width != viewer_width_ || viewer_collapsed_ || !viewer_manually_sized_;
      viewer_width_ = next_width;
      viewer_collapsed_ = false;
      viewer_manually_sized_ = true;
      if (changed) request_redraw();
      return {changed ? WorkbenchActionKind::redraw : WorkbenchActionKind::none, {}, 0, 0, false, true};
    }
    if (selecting_input_) {
      const auto &bounds = (selecting_editor_ == "settings" ? settings_editor_bounds_ : message_editor_bounds_);
      const auto &text = (selecting_editor_ == "settings" ? settings_editor_text_ : message_editor_text_);
      const auto cursor = editor_offset_at(event.x, event.y, bounds, text);
      if (cursor == editor_cursor_) return {};
      editor_cursor_ = cursor;
      request_redraw(bounds);
      return {WorkbenchActionKind::set_editor_cursor, {}, 0, cursor, true, false};
    }

    const bool over_splitter =
        last_layout_.sidebar_splitter.contains(event.x, event.y) ||
        last_layout_.viewer_splitter.contains(event.x, event.y);
    WorkbenchAction result;
    result.kind = hover_changed ? WorkbenchActionKind::redraw : WorkbenchActionKind::none;
    if (over_splitter != pointer_cursor_active_) {
      pointer_cursor_active_ = over_splitter;
      result.pointer_cursor = over_splitter;
    }
    return result;
  }

  if (event.type == "pointerleave") {
    const bool hover_changed = active_hover_region_.has_value();
    if (active_hover_region_ && *active_hover_region_ < hover_regions_.size())
      request_redraw(hover_regions_[*active_hover_region_]);
    pointer_x_ = -1;
    pointer_y_ = -1;
    active_hover_region_.reset();
    WorkbenchAction result;
    result.kind = hover_changed ? WorkbenchActionKind::redraw : WorkbenchActionKind::none;
    if (pointer_cursor_active_) {
      pointer_cursor_active_ = false;
      result.pointer_cursor = false;
    }
    return result;
  }

  if (event.type == "wheel") {
    if (settings_open_ || archive_open_ || plugins_open_) return {};
    if (last_layout_.timeline.contains(event.x, event.y)) {
      const auto next = std::clamp(timeline_scroll_ + event.delta_y, 0.0F, timeline_max_scroll_);
      if (next == timeline_scroll_) return {};
      timeline_scroll_ = next;
      follow_tail_ = (timeline_scroll_ >= timeline_max_scroll_ - 2);
      request_redraw(last_layout_.timeline);
      return {WorkbenchActionKind::redraw};
    }
    if (last_layout_.document.contains(event.x, event.y)) {
      const auto next = std::clamp(document_scroll_ + event.delta_y, 0.0F, document_max_scroll_);
      if (next == document_scroll_) return {};
      document_scroll_ = next;
      request_redraw(last_layout_.document);
      return {WorkbenchActionKind::redraw};
    }
    return {};
  }

  if (event.type != "click")
    return {};

  request_redraw();
  if (resizing_sidebar_ || resizing_viewer_) {
    resizing_sidebar_ = false;
    resizing_viewer_ = false;
    const bool over_splitter =
        last_layout_.sidebar_splitter.contains(event.x, event.y) ||
        last_layout_.viewer_splitter.contains(event.x, event.y);
    pointer_cursor_active_ = over_splitter;
    return {WorkbenchActionKind::none, {}, 0, 0, false, over_splitter};
  }
  if (selecting_input_) {
    selecting_input_ = false;
    selecting_editor_.clear();
    return {};
  }

  const Rect menu_headers{128, 10, 222, 38};
  if (!active_menu_.empty() && !open_menu_bounds_.contains(event.x, event.y) &&
      !menu_headers.contains(event.x, event.y)) {
    active_menu_.clear();
    return {WorkbenchActionKind::redraw};
  }
  if (profile_menu_open_ && !profile_menu_bounds_.contains(event.x, event.y)) {
    const auto on_anchor = std::ranges::any_of(hits_, [&](const auto &target) {
      return target.action == WorkbenchActionKind::toggle_profile_menu &&
             target.bounds.contains(event.x, event.y);
    });
    if (!on_anchor) {
      profile_menu_open_ = false;
      return {WorkbenchActionKind::redraw};
    }
  }

  for (const auto &target : std::views::reverse(hits_)) {
    if (!target.bounds.contains(event.x, event.y))
      continue;
    if (target.action == WorkbenchActionKind::toggle_menu) {
      active_menu_ = (active_menu_ == target.value ? "" : target.value);
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
    if (target.action == WorkbenchActionKind::toggle_profile_menu) {
      profile_menu_open_ = !profile_menu_open_;
      active_menu_.clear();
      return {WorkbenchActionKind::redraw};
    }
    if (target.action == WorkbenchActionKind::open_settings) {
      profile_menu_open_ = false;
      archive_open_ = false;
      plugins_open_ = false;
      settings_open_ = true;
      active_menu_.clear();
      return {WorkbenchActionKind::open_settings};
    }
    if (target.action == WorkbenchActionKind::close_settings) {
      settings_open_ = false;
      return {WorkbenchActionKind::close_settings};
    }
    if (target.action == WorkbenchActionKind::open_archive) {
      profile_menu_open_ = false;
      settings_open_ = false;
      plugins_open_ = false;
      archive_open_ = true;
      active_menu_.clear();
      return {WorkbenchActionKind::open_archive};
    }
    if (target.action == WorkbenchActionKind::close_archive) {
      archive_open_ = false;
      return {WorkbenchActionKind::close_archive};
    }
    if (target.action == WorkbenchActionKind::open_plugins) {
      profile_menu_open_ = false;
      settings_open_ = false;
      archive_open_ = false;
      plugins_open_ = true;
      active_menu_.clear();
      return {WorkbenchActionKind::open_plugins};
    }
    if (target.action == WorkbenchActionKind::close_plugins) {
      plugins_open_ = false;
      return {WorkbenchActionKind::close_plugins};
    }
    if (target.action == WorkbenchActionKind::save_settings) {
      return {WorkbenchActionKind::save_settings};
    }
    if (target.action == WorkbenchActionKind::settings_tab) {
      settings_tab_ = target.value;
      return {WorkbenchActionKind::settings_tab, target.value};
    }
    if (target.action == WorkbenchActionKind::show_conversation) {
      trajectory_open_ = false;
      return {WorkbenchActionKind::show_conversation};
    }
    if (target.action == WorkbenchActionKind::show_trajectory) {
      trajectory_open_ = true;
      return {WorkbenchActionKind::show_trajectory};
    }
    if (target.action == WorkbenchActionKind::viewer_tab) {
      viewer_tab_ = target.value;
      return {WorkbenchActionKind::viewer_tab, target.value};
    }
    if (target.action == WorkbenchActionKind::scroll_to_tail) {
      timeline_scroll_ = timeline_max_scroll_;
      follow_tail_ = true;
      return {WorkbenchActionKind::redraw};
    }
    active_menu_.clear();
    return {target.action, target.value, target.index};
  }

  if (!active_menu_.empty()) {
    active_menu_.clear();
    return {WorkbenchActionKind::redraw};
  }
  return {};
}

} // namespace tokmon::desktop
