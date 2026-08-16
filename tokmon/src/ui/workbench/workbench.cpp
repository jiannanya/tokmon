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

// Tokmon's desktop skin follows the reference system: cool paper-white
// surfaces, restrained blue focus, one-pixel neutral borders and soft depth.
// Spacing carries hierarchy; saturated color is reserved for state.
constexpr Color app_background{248, 248, 247, 255};
constexpr Color sidebar_background{251, 251, 250, 255};
constexpr Color panel{255, 255, 255, 255};
constexpr Color ink{27, 30, 36, 255};
constexpr Color secondary{82, 88, 101, 255};
constexpr Color muted{137, 143, 156, 255};
constexpr Color hairline{229, 230, 232, 255};
constexpr Color hover_fill{246, 247, 248, 255};
constexpr Color hover_border{207, 211, 217, 255};
constexpr Color selected_fill{240, 242, 244, 255};
constexpr Color accent{45, 103, 235, 255};
constexpr Color success{44, 169, 93, 255};
constexpr Color warning{230, 128, 31, 255};
constexpr Color danger{228, 69, 79, 255};
// Tokmon keeps the configured 1.25 product zoom for crisp HiDPI text. The
// reference artwork, however, uses a compact 1x information density. Native
// component metrics therefore use 0.8 logical units per design pixel.
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
                 {}, accent);
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
      if (marker != std::string::npos && marker < 4) {
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
    return "";
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
  return "新建 Arche Agent 会话";
}

struct TrajectoryVisual {
  std::string_view label;
  Color color;
  int lane;
};

TrajectoryVisual trajectory_visual(std::string_view type) {
  if (type.starts_with("user/"))
    return {"USER", {67, 126, 222, 255}, 0};
  if (type.starts_with("assistant/"))
    return {"ASSISTANT", {126, 91, 171, 255}, 1};
  if (type.starts_with("model/") || type.starts_with("request/"))
    return {"MODEL", {92, 173, 126, 255}, 1};
  if (type.starts_with("tool/"))
    return {"TOOL", {221, 139, 38, 255}, 2};
  if (type.starts_with("context/"))
    return {"CONTEXT", {62, 166, 125, 255}, 0};
  if (type.starts_with("approval/"))
    return {"APPROVAL", {220, 119, 41, 255}, 2};
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
  return {};
}

bool visible_conversation_item(const ConversationItem &item) {
  // Canonical control-plane events belong in the trajectory inspector. They
  // add noise to the human conversation and were the source of the large raw
  // JSON blocks visible in the previous UI.
  return !(item.kind == ItemKind::status && item.title.starts_with("Event /"));
}

float item_height(const ConversationItem &item, float width) {
  // A retained card's height must be derived from its explicit line breaks as
  // well as wrapping. Counting only the total byte length made four short
  // numbered lines look like a single paragraph and clipped the final rows.
  std::size_t visual_lines = 0;
  const auto content_width = std::min(width, dp(500.0F)) - dp(28.0F);
  for (const auto &line : split_lines(item.content))
    visual_lines += estimated_rows(line, content_width, dp(10.0F));
  visual_lines = std::max<std::size_t>(1, visual_lines);
  switch (item.kind) {
  case ItemKind::user:
    return dp(75) + static_cast<float>(visual_lines) * dp(12);
  case ItemKind::assistant:
    return dp(18) + markdown_height(item.content, width - dp(92));
  case ItemKind::tool:
    return dp(58) +
           static_cast<float>(std::min<std::size_t>(visual_lines, 4)) * dp(17);
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
  } else if (name == "pin") {
    surface.line(x - 3, y - 6, x + 5, y + 2, color, 1.25F);
    surface.line(x - 5, y + 1, x + 2, y - 6, color, 1.25F);
    surface.line(x - 5, y + 1, x - 1, y + 3, color, 1.25F);
    surface.line(x - 1, y + 3, x + 5, y + 2, color, 1.25F);
    surface.line(x, y + 3, x - 5, y + 8, color, 1.25F);
  } else if (name == "history") {
    surface.stroke_rect({x - 6, y - 6, 12, 12}, color, 1.2F, 6);
    surface.line(x - 6, y - 1, x - 9, y - 4, color, 1.2F);
    surface.line(x - 6, y - 1, x - 3, y - 3, color, 1.2F);
    surface.line(x, y - 4, x, y, color, 1.2F);
    surface.line(x, y, x + 3, y + 2, color, 1.2F);
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
  } else if (name == "settings") {
    surface.stroke_rect({x - 5.5F, y - 5.5F, 11, 11}, color, 1.2F, 5.5F);
    surface.stroke_rect({x - 1.8F, y - 1.8F, 3.6F, 3.6F}, color, 1.1F,
                        1.8F);
    surface.line(x, y - 8, x, y - 5, color, 1.2F);
    surface.line(x, y + 5, x, y + 8, color, 1.2F);
    surface.line(x - 8, y, x - 5, y, color, 1.2F);
    surface.line(x + 5, y, x + 8, y, color, 1.2F);
    surface.line(x - 5.7F, y - 5.7F, x - 3.8F, y - 3.8F, color, 1.2F);
    surface.line(x + 3.8F, y + 3.8F, x + 5.7F, y + 5.7F, color, 1.2F);
    surface.line(x + 5.7F, y - 5.7F, x + 3.8F, y - 3.8F, color, 1.2F);
    surface.line(x - 3.8F, y + 3.8F, x - 5.7F, y + 5.7F, color, 1.2F);
  } else if (name == "sliders") {
    surface.line(x - 7, y - 4, x + 7, y - 4, color, 1.15F);
    surface.line(x - 7, y + 4, x + 7, y + 4, color, 1.15F);
    surface.fill_circle(x - 2, y - 4, 2, color);
    surface.fill_circle(x + 3, y + 4, 2, color);
  } else if (name == "model") {
    surface.stroke_rect({x - 7, y - 6, 14, 12}, color, 1.2F, 3);
    surface.fill_circle(x - 3, y, 1.5F, color);
    surface.fill_circle(x + 3, y, 1.5F, color);
    surface.line(x, y - 9, x, y - 6, color, 1.2F);
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
    surface.line(x - 6, y - 3, x, y + 1, color, 1.1F);
    surface.line(x, y + 1, x + 6, y - 3, color, 1.1F);
    surface.line(x, y + 1, x, y + 8, color, 1.1F);
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
  } else if (name == "microphone") {
    surface.stroke_rect({x - 3, y - 7, 6, 11}, color, 1.2F, 3);
    surface.line(x - 6, y - 1, x - 6, y + 1, color, 1.2F);
    surface.line(x - 6, y + 1, x - 3, y + 5, color, 1.2F);
    surface.line(x - 3, y + 5, x + 3, y + 5, color, 1.2F);
    surface.line(x + 3, y + 5, x + 6, y + 1, color, 1.2F);
    surface.line(x + 6, y + 1, x + 6, y - 1, color, 1.2F);
    surface.line(x, y + 5, x, y + 8, color, 1.2F);
    surface.line(x - 4, y + 8, x + 4, y + 8, color, 1.2F);
  } else if (name == "paper-plane") {
    surface.line(x - 7, y - 5, x + 7, y - 8, color, 1.4F);
    surface.line(x + 7, y - 8, x + 3, y + 7, color, 1.4F);
    surface.line(x + 3, y + 7, x - 1, y + 2, color, 1.4F);
    surface.line(x - 1, y + 2, x - 7, y - 5, color, 1.4F);
    surface.line(x - 1, y + 2, x + 7, y - 8, color, 1.2F);
  } else if (name == "play") {
    surface.line(x - 4, y - 6, x + 5, y, color, 1.25F);
    surface.line(x + 5, y, x - 4, y + 6, color, 1.25F);
    surface.line(x - 4, y + 6, x - 4, y - 6, color, 1.25F);
  } else if (name == "trash") {
    surface.stroke_rect({x - 5, y - 4, 10, 11}, color, 1.1F, 2);
    surface.line(x - 7, y - 7, x + 7, y - 7, color, 1.1F);
    surface.line(x - 2, y - 9, x + 2, y - 9, color, 1.1F);
    surface.line(x - 2, y - 2, x - 2, y + 4, color, 1.0F);
    surface.line(x + 2, y - 2, x + 2, y + 4, color, 1.0F);
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
  text(settings.theme);
  text(settings.provider_id);
  text(settings.provider_name);
  text(settings.provider_kind);
  text(settings.endpoint);
  text(settings.api_key_env);
  text(settings.model);
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
    // Keep the overview as the initial dock tab. The document is primed for
    // preview, but opening the application should match the design's project
    // dashboard instead of jumping directly into a file.
    viewer_tab_ = "workspace";
  } else {
    selected_document_ = "Welcome";
    document_lines_ = {"# Tokmon", "", "Arche Agent OS 工作台已就绪。",
                       "在左侧创建会话，在中间与 Snow 协作。"};
  }
}

WorkbenchView::~WorkbenchView() = default;

WorkbenchLayout WorkbenchView::layout(float width, float height) const {
  WorkbenchLayout result;
  result.compact_sidebar = sidebar_collapsed_ ||
                           (width < sidebar_compact_breakpoint &&
                            !sidebar_manually_sized_);
  const auto sidebar_content_reserve =
      width >= viewer_visible_breakpoint && !viewer_collapsed_ ? 940.0F
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
      const auto relative = std::filesystem::relative(entry.path(), workspace_);
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
  if (selected_document_ != relative)
    return;
  if (open_documents_.empty()) {
    selected_document_ = "Welcome";
    document_lines_ = {"# Tokmon", "", "选择工作区文件以开始预览。"};
  } else {
    open_document(open_documents_[std::min(index, open_documents_.size() - 1)]);
  }
}

std::size_t
WorkbenchView::editor_offset_at(float x, float y, const Rect &editor_bounds,
                                std::string_view editor_text) const {
  constexpr float row_height = dp(18.0F);
  auto cursor_x = editor_bounds.x;
  auto cursor_y = editor_bounds.y;
  std::size_t offset = 0;
  std::size_t nearest = 0;
  float nearest_distance = std::numeric_limits<float>::max();
  while (offset <= editor_text.size()) {
    const auto distance =
        std::abs(cursor_y - y) * 4.0F + std::abs(cursor_x - x);
    if (distance < nearest_distance) {
      nearest_distance = distance;
      nearest = offset;
    }
    if (offset == editor_text.size())
      break;
    const auto first = static_cast<unsigned char>(editor_text[offset]);
    const std::size_t width = first < 0x80U   ? 1
                              : first < 0xe0U ? 2
                              : first < 0xf0U ? 3
                                              : 4;
    if (editor_text[offset] == '\n') {
      cursor_x = editor_bounds.x;
      cursor_y += row_height;
    } else {
      const auto advance =
          first < 0x80U
              ? (editor_text[offset] == ' ' ? dp(4.2F) : dp(7.2F))
              : dp(13.0F);
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

bool WorkbenchView::hovered(const Rect &bounds) noexcept {
  hover_regions_.push_back(bounds);
  return bounds.contains(pointer_x_, pointer_y_);
}

std::optional<std::size_t>
WorkbenchView::hover_region_at(float x, float y) const noexcept {
  for (std::size_t index = hover_regions_.size(); index > 0; --index) {
    if (hover_regions_[index - 1].contains(x, y)) return index - 1;
  }
  return std::nullopt;
}

void WorkbenchView::request_redraw(Rect damage) noexcept {
  if (full_redraw_pending_) return;
  if (damage.empty()) {
    full_redraw_pending_ = true;
    pending_damage_.reset();
    return;
  }
  // Include antialiasing/stroke fringes in the invalidated area.
  damage = {damage.x - 3, damage.y - 3, damage.width + 6,
            damage.height + 6};
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
  // The retained HTML/CSS shell paints the structural surfaces. The code
  // below consists only of Tokmon native components mounted at data-native
  // boundaries (timeline, editor, file tree and overlays).
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
        primary ? (is_hovered ? Color{64, 67, 73, 255} : Color{44, 46, 50, 255})
                : (is_hovered ? hover_fill : panel);
    surface.fill_rect(bounds, background, bounds.height * 0.45F);
    surface.stroke_rect(
        bounds, primary ? background : (is_hovered ? hover_border : hairline),
        1, bounds.height * 0.45F);
    label(surface, text,
          {bounds.x + 8, bounds.y + 6, bounds.width - 16, bounds.height - 8},
          12, primary ? Color{255, 255, 255, 255} : text_color, 500, 1,
          white::TextAlign::center);
    add_hit(bounds, action);
  };

  // Reference-aligned application bar: a generous 64px quiet strip, compact
  // commands, and settings/account controls grouped with native window chrome.
  surface.fill_rect(last_layout_.menu_bar, app_background);
  draw_icon(surface, "branch", dp(29), dp(32), ink);
  label(surface, "Tokmon", {dp(64), dp(19), dp(88), dp(28)}, 18, ink, 700);
  constexpr Rect menu_items[] = {{dp(160), dp(13), dp(54), dp(38)},
                                 {dp(218), dp(13), dp(54), dp(38)},
                                 {dp(276), dp(13), dp(54), dp(38)},
                                 {dp(334), dp(13), dp(54), dp(38)}};
  constexpr std::string_view menu_ids[] = {"file", "edit", "view", "help"};
  for (std::size_t index = 0; index < std::size(menu_items); ++index) {
    if (hovered(menu_items[index]) || active_menu_ == menu_ids[index])
      surface.fill_rect(menu_items[index], hover_fill, dp(6));
  }
  label(surface, "文件", menu_items[0], 15, ink, 450, 1,
        white::TextAlign::center);
  label(surface, "编辑", menu_items[1], 15, ink, 450, 1,
        white::TextAlign::center);
  label(surface, "视图", menu_items[2], 15, ink, 450, 1,
        white::TextAlign::center);
  label(surface, "帮助", menu_items[3], 15, ink, 450, 1,
        white::TextAlign::center);
  for (std::size_t index = 0; index < std::size(menu_items); ++index)
    hits_.push_back({menu_items[index],
                     WorkbenchActionKind::toggle_menu,
                     {},
                     std::string(menu_ids[index])});
  const Rect settings_button{width - dp(256), dp(13), dp(40), dp(38)};
  if (hovered(settings_button))
    surface.fill_rect(settings_button, hover_fill, dp(10));
  draw_icon(surface, "settings", settings_button.x + dp(20),
            settings_button.y + dp(19), ink);
  add_hit(settings_button, WorkbenchActionKind::open_settings);
  const Rect account_button{width - dp(210), dp(10), dp(46), dp(44)};
  if (hovered(account_button) || profile_menu_open_)
    surface.fill_rect(account_button, hover_fill, dp(12));
  surface.fill_circle(account_button.x + dp(23), account_button.y + dp(22),
                      dp(16),
                      {232, 224, 211, 255});
  surface.fill_circle(account_button.x + dp(23), account_button.y + dp(18),
                      dp(5),
                      {55, 52, 49, 255});
  surface.fill_rect({account_button.x + dp(15), account_button.y + dp(24),
                     dp(16), dp(10)},
                    {55, 52, 49, 255}, dp(7));
  add_hit(account_button, WorkbenchActionKind::toggle_profile_menu);
  surface.line(width - dp(158), dp(14), width - dp(158), dp(50), hairline);
  const Rect minimize{width - dp(132), 0, dp(44), dp(63)};
  const Rect maximize{width - dp(88), 0, dp(44), dp(63)};
  const Rect window_close_bounds{width - dp(44), 0, dp(44), dp(63)};
  if (hovered(minimize))
    surface.fill_rect(minimize, hover_fill);
  if (hovered(maximize))
    surface.fill_rect(maximize, hover_fill);
  if (hovered(window_close_bounds))
    surface.fill_rect(window_close_bounds, {225, 75, 75, 255});
  draw_icon(surface, "window-minimize", minimize.x + dp(22), dp(31),
            secondary);
  draw_icon(surface,
            frame.window_maximized ? "window-restore" : "window-maximize",
            maximize.x + dp(22), dp(31), secondary);
  draw_icon(surface, "window-close", window_close_bounds.x + dp(22), dp(31),
            hovered(window_close_bounds) ? Color{255, 255, 255, 255}
                                         : secondary);
  add_hit(minimize, WorkbenchActionKind::window_minimize);
  add_hit(maximize, WorkbenchActionKind::window_toggle_maximize);
  add_hit(window_close_bounds, WorkbenchActionKind::window_close);

  // Project navigator. Sessions are grouped below the active workspace and
  // product destinations read like project tree entries rather than a toolbar.
  if (last_layout_.sidebar.width > 0) {
    surface.fill_rect({last_layout_.sidebar.x + 1, last_layout_.sidebar.y + 2,
                       last_layout_.sidebar.width,
                       last_layout_.sidebar.height},
                      {36, 42, 52, 10}, panel_radius);
    surface.fill_rect(last_layout_.sidebar, sidebar_background, panel_radius);
    surface.stroke_rect(last_layout_.sidebar, hairline, 1, panel_radius);
    const float side_x = last_layout_.sidebar.x;
    const float side_y = last_layout_.sidebar.y;
    const float side_w = last_layout_.sidebar.width;
    const float side_h = last_layout_.sidebar.height;
    const bool compact = last_layout_.compact_sidebar;
    const Rect collapse{side_x + side_w - dp(50), side_y + dp(15), dp(36),
                        dp(36)};
    if (hovered(collapse)) surface.fill_rect(collapse, hover_fill, dp(9));
    draw_icon(surface, "panel-left", collapse.x + dp(18),
              collapse.y + dp(18),
              secondary);
    add_hit(collapse, WorkbenchActionKind::toggle_left_panel);

    if (compact) {
      const Rect create{side_x + dp(16), side_y + dp(70), dp(40), dp(40)};
      if (hovered(create)) surface.fill_rect(create, hover_fill, dp(10));
      draw_icon(surface, "plus", create.x + dp(20), create.y + dp(20), ink);
      add_hit(create, WorkbenchActionKind::new_session);
      const Rect plugin{side_x + dp(16), side_y + dp(122), dp(40), dp(40)};
      if (hovered(plugin)) surface.fill_rect(plugin, hover_fill, dp(10));
      draw_icon(surface, "plugin", plugin.x + dp(20), plugin.y + dp(20),
                secondary);
      add_hit(plugin, WorkbenchActionKind::open_plugins);
      const Rect archive{side_x + 16, side_y + side_h - 56, 40, 40};
      if (hovered(archive)) surface.fill_rect(archive, hover_fill, 10);
      draw_icon(surface, "file", archive.x + 20, archive.y + 20, secondary);
      add_hit(archive, WorkbenchActionKind::open_archive);
      session_scroll_ = 0;
      session_max_scroll_ = 0;
    } else {
      const Rect search{side_x + dp(17), side_y + dp(64), side_w - dp(34),
                        dp(40)};
      surface.fill_rect(search, panel, dp(9));
      surface.stroke_rect(search,
                          frame.filter_focused ? accent : hairline,
                          frame.filter_focused ? 1.3F : 1.0F, dp(9));
      draw_icon(surface, "search", search.x + dp(17), search.y + dp(20),
                muted);
      label(surface, "搜索项目或会话…",
            {search.x + dp(36), search.y + dp(9), search.width - dp(90),
             dp(22)},
            13, muted,
            400);
      const Rect shortcut{search.x + search.width - dp(58), search.y + dp(8),
                          dp(48), dp(24)};
      surface.fill_rect(shortcut, {249, 250, 251, 255}, dp(6));
      surface.stroke_rect(shortcut, hairline, 1, dp(6));
      label(surface, "Ctrl  K", shortcut, 9, muted, 500, 1,
            white::TextAlign::center);
      add_hit(search, WorkbenchActionKind::focus_filter);

      const auto section_header = [&](std::string_view title, float y,
                                      bool create_session) {
        draw_icon(surface, "down", side_x + dp(20), y + dp(11), secondary);
        label(surface, title,
              {side_x + dp(36), y, side_w - dp(112), dp(22)}, 14, ink, 650);
        const Rect plus{side_x + side_w - dp(76), y - dp(5), dp(28), dp(28)};
        const Rect more{side_x + side_w - dp(42), y - dp(5), dp(28), dp(28)};
        if (hovered(plus)) surface.fill_rect(plus, hover_fill, dp(7));
        if (hovered(more)) surface.fill_rect(more, hover_fill, dp(7));
        draw_icon(surface, "plus", plus.x + dp(14), plus.y + dp(14), secondary);
        label(surface, "•••", more, 11, secondary, 600, 1,
              white::TextAlign::center);
        if (create_session) add_hit(plus, WorkbenchActionKind::new_session);
      };
      const auto tree_row = [&](Rect row, std::string_view icon,
                                std::string_view text, bool selected,
                                WorkbenchActionKind action,
                                std::string value = {}) {
        if (selected || hovered(row))
          surface.fill_rect(row, selected ? selected_fill : hover_fill, dp(8));
        draw_icon(surface, icon, row.x + dp(28), row.y + row.height / 2,
                  selected ? ink : secondary);
        label(surface, text,
              {row.x + dp(46), row.y + dp(6), row.width - dp(54),
               row.height - dp(9)},
              13,
              selected ? ink : secondary, selected ? 600 : 450);
        hits_.push_back({row, action, {}, std::move(value)});
      };

      float y = side_y + dp(130);
      section_header("核心项目", y, true);
      y += dp(36);
      const Rect workspace_row{side_x + dp(12), y, side_w - dp(24), dp(32)};
      draw_icon(surface, "chevron", workspace_row.x + dp(17),
                workspace_row.y + dp(16), secondary);
      draw_icon(surface, "folder", workspace_row.x + dp(43),
                workspace_row.y + dp(16), secondary);
      label(surface, workspace_.filename().string(),
            {workspace_row.x + dp(62), workspace_row.y + dp(6),
             workspace_row.width - dp(70), dp(21)},
            13, ink, 600);
      y += dp(34);
      std::size_t shown_sessions = 0;
      for (const auto &session : frame.session_items()) {
        if (shown_sessions++ >= 3) break;
        const Rect row{side_x + dp(46), y, side_w - dp(58), dp(34)};
        tree_row(row, "chat", session.title, session.id == frame.session_id,
                 WorkbenchActionKind::switch_session, session.id);
        y += dp(36);
      }
      if (shown_sessions == 0) {
        const Rect row{side_x + dp(46), y, side_w - dp(58), dp(34)};
        tree_row(row, "chat", "新建会话", true,
                 WorkbenchActionKind::new_session);
        y += dp(36);
      }

      y += dp(18);
      section_header("研究与实验", y, false);
      y += dp(36);
      const Rect playground{side_x + dp(12), y, side_w - dp(24), dp(32)};
      draw_icon(surface, "chevron", playground.x + dp(17),
                playground.y + dp(16),
                secondary);
      draw_icon(surface, "folder", playground.x + dp(43),
                playground.y + dp(16),
                secondary);
      label(surface, "plugin-playground",
            {playground.x + dp(62), playground.y + dp(6),
             playground.width - dp(70), dp(21)},
            13, ink, 550);
      y += dp(34);
      tree_row({side_x + dp(46), y, side_w - dp(58), dp(34)}, "plugin",
               "插件通信机制设计", false, WorkbenchActionKind::open_plugins);
      y += dp(36);
      tree_row({side_x + dp(46), y, side_w - dp(58), dp(34)}, "pulse",
               "原型验证：窗口编排", false,
               WorkbenchActionKind::diagnostics);

      y += dp(58);
      section_header("个人工作", y, false);
      y += dp(36);
      const Rect notes{side_x + dp(12), y, side_w - dp(24), dp(32)};
      draw_icon(surface, "chevron", notes.x + dp(17), notes.y + dp(16),
                secondary);
      draw_icon(surface, "folder", notes.x + dp(43), notes.y + dp(16),
                secondary);
      label(surface, "notes",
            {notes.x + dp(62), notes.y + dp(6), notes.width - dp(70), dp(21)},
            13, ink, 550);
      y += dp(34);
      tree_row({side_x + dp(46), y, side_w - dp(58), dp(34)}, "chat", "待办清单",
               false, WorkbenchActionKind::redraw);
      y += dp(36);
      tree_row({side_x + dp(46), y, side_w - dp(58), dp(34)}, "chat", "读书笔记",
               false, WorkbenchActionKind::redraw);

      session_scroll_ = 0;
      session_max_scroll_ = 0;
      surface.line(side_x + dp(18), side_y + side_h - 68,
                   side_x + side_w - dp(18),
                   side_y + side_h - 68, hairline);
      const Rect archive{side_x + dp(16), side_y + side_h - 56,
                         side_w - dp(32), 42};
      if (hovered(archive)) surface.fill_rect(archive, hover_fill, 9);
      draw_icon(surface, "file", archive.x + dp(18), archive.y + 21,
                secondary);
      label(surface, "归档项目",
            {archive.x + dp(38), archive.y + 9, archive.width - dp(70), 23}, 13,
            secondary, 550);
      draw_icon(surface, "chevron", archive.x + archive.width - dp(18),
                archive.y + 21, muted);
      add_hit(archive, WorkbenchActionKind::open_archive);
    }
  }

  // Conversation panel and header.
  const auto &conversation = last_layout_.conversation;
  surface.fill_rect({conversation.x + 1, conversation.y + 2,
                     conversation.width, conversation.height},
                    {36, 42, 52, 10}, panel_radius);
  surface.fill_rect(conversation, panel, panel_radius);
  surface.stroke_rect(conversation, hairline, 1, panel_radius);
  const float header_bottom = conversation.y + dp(62);
  surface.line(conversation.x, header_bottom,
               conversation.x + conversation.width, header_bottom, hairline);
  const Rect conversation_tab{conversation.x + dp(16), conversation.y + 11,
                              dp(60), dp(48)};
  const Rect trajectory_tab{conversation.x + dp(80), conversation.y + 11,
                            dp(60), dp(48)};
  if (hovered(conversation_tab))
    surface.fill_rect(conversation_tab, hover_fill, dp(8));
  if (hovered(trajectory_tab))
    surface.fill_rect(trajectory_tab, hover_fill, dp(8));
  if (trajectory_open_) {
    label(surface, "对话", conversation_tab, 13, secondary, 450, 1,
          white::TextAlign::center);
    label(surface, "轨迹", trajectory_tab, 13, ink, 650, 1,
          white::TextAlign::center);
    surface.fill_rect({trajectory_tab.x + dp(12), header_bottom - dp(2),
                       trajectory_tab.width - dp(24), dp(2)},
                      accent, dp(1));
  } else {
    label(surface, normalized_title(frame),
          {conversation.x + dp(20), conversation.y + 20,
           conversation.width - dp(214), dp(26)},
          15, ink, 650);
  }
  if (trajectory_open_) {
    add_hit(conversation_tab, WorkbenchActionKind::show_conversation);
    add_hit(trajectory_tab, WorkbenchActionKind::show_trajectory);
  }
  const Rect fork_button{conversation.x + conversation.width - dp(122),
                         conversation.y + dp(18), dp(34), dp(34)};
  if (hovered(fork_button))
    surface.fill_rect(fork_button, hover_fill, dp(9));
  draw_icon(surface, "pin", fork_button.x + dp(17),
            fork_button.y + dp(17), secondary);
  add_hit(fork_button, WorkbenchActionKind::fork_session);
  const Rect trace_button{conversation.x + conversation.width - dp(88),
                          conversation.y + dp(18), dp(34), dp(34)};
  if (hovered(trace_button))
    surface.fill_rect(trace_button, hover_fill, dp(9));
  draw_icon(surface, "history", trace_button.x + dp(17),
            trace_button.y + dp(17),
            trajectory_open_ ? accent : secondary);
  add_hit(trace_button, trajectory_open_ ? WorkbenchActionKind::show_conversation
                                         : WorkbenchActionKind::show_trajectory);
  const Rect inspect_button{conversation.x + conversation.width - dp(55),
                            conversation.y + dp(18), dp(34), dp(34)};
  if (hovered(inspect_button))
    surface.fill_rect(inspect_button, hover_fill, dp(9));
  label(surface, "•••",
        {inspect_button.x, inspect_button.y + dp(7), inspect_button.width,
         dp(20)},
        11,
        secondary, 600, 1, white::TextAlign::center);
  add_hit(inspect_button, WorkbenchActionKind::inspect_composition);
  const auto conversation_content_hit_start = hits_.size();

  // Conversation timeline.
  const auto &timeline = last_layout_.timeline;
  float content_height = dp(10);
  bool has_visible_item = false;
  for (const auto &item : frame.conversation_items()) {
    if (!visible_conversation_item(item)) continue;
    if (has_visible_item) content_height += dp(8);
    content_height += item_height(item, timeline.width - dp(64));
    has_visible_item = true;
  }
  timeline_max_scroll_ = std::max(0.0F, content_height - timeline.height);
  if (frame.conversation_items().size() != previous_item_count_) {
    if (follow_tail_ && frame.settings.auto_scroll)
      timeline_scroll_ = timeline_max_scroll_;
    previous_item_count_ = frame.conversation_items().size();
  }
  timeline_scroll_ = std::clamp(timeline_scroll_, 0.0F, timeline_max_scroll_);
  surface.push_clip(timeline);
  float item_y = timeline.y + dp(10) - timeline_scroll_;
  if (frame.conversation_items().empty()) {
    const float welcome_width = std::min(dp(390.0F), timeline.width - dp(70));
    const Rect welcome{timeline.x + (timeline.width - welcome_width) / 2,
                       timeline.y + dp(72), welcome_width, dp(210)};
    surface.fill_circle(welcome.x + welcome.width / 2, welcome.y + dp(24),
                        dp(20),
                        {42, 44, 48, 255});
    label(surface, "A",
          {welcome.x + welcome.width / 2 - dp(12), welcome.y + dp(12), dp(24),
           dp(24)},
          14,
          {255, 255, 255, 255}, 700, 1, white::TextAlign::center);
    label(surface, "Arche Agent OS 已就绪",
          {welcome.x, welcome.y + dp(56), welcome.width, dp(30)}, 18, ink, 650,
          1,
          white::TextAlign::center);
    label(surface,
          "描述你的目标。Snow "
          "会记录完整轨迹，按策略调用能力，并把结果投影到这里。",
          {welcome.x + dp(20), welcome.y + dp(92), welcome.width - dp(40),
           dp(52)},
          13,
          secondary, 400, 3, white::TextAlign::center);
    const Rect suggestion{welcome.x + dp(34), welcome.y + dp(160),
                          welcome.width - dp(68), dp(34)};
    surface.fill_rect(
        suggestion,
        hovered(suggestion) ? hover_fill : Color{248, 248, 247, 255}, dp(8));
    surface.stroke_rect(suggestion,
                        hovered(suggestion) ? hover_border : hairline, 1,
                        dp(8));
    label(surface, "检查当前工作区并给出下一步建议", suggestion, 12, secondary,
          450, 1, white::TextAlign::center);
    hits_.push_back({suggestion,
                     WorkbenchActionKind::set_message_input,
                     {},
                     "检查当前工作区并给出下一步建议"});
  } else {
    for (const auto &item : frame.conversation_items()) {
      if (!visible_conversation_item(item)) continue;
      const float card_height = item_height(item, timeline.width - dp(64));
      if (item_y + card_height > timeline.y &&
          item_y < timeline.y + timeline.height) {
        if (item.kind == ItemKind::user) {
          const auto rows = estimated_rows(
              item.content, std::min(timeline.width * 0.72F, dp(520.0F)),
              dp(13));
          const float bubble_width =
              std::clamp(dp(36.0F) + visual_units(item.content) * dp(7.7F),
                         dp(132.0F),
                         std::min(timeline.width * 0.72F, dp(520.0F)));
          const float bubble_height =
              dp(26.0F) + static_cast<float>(rows) * dp(20.0F);
          const float avatar_x = timeline.x + timeline.width - dp(52);
          const Rect bubble{avatar_x - bubble_width - dp(37), item_y + dp(24),
                            bubble_width, bubble_height};
          label(surface, "用户",
                {bubble.x + dp(12), item_y + dp(3), dp(48), dp(20)}, 10, ink,
                600);
          label(surface, clock_label(item.metadata),
                {bubble.x + dp(58), item_y + dp(3), dp(52), dp(20)}, 9, muted,
                430);
          surface.fill_circle(avatar_x, item_y + dp(19), dp(17),
                              {238, 230, 218, 255});
          surface.fill_circle(avatar_x, item_y + dp(15), dp(5),
                              {67, 58, 52, 255});
          surface.fill_rect({avatar_x - dp(8), item_y + dp(21), dp(16), dp(10)},
                            {67, 58, 52, 255}, dp(7));
          surface.fill_rect({bubble.x + dp(1), bubble.y + dp(3), bubble.width,
                             bubble.height},
                            {63, 102, 164, 16}, dp(13));
          surface.fill_rect(bubble, {238, 246, 255, 255}, dp(13));
          surface.stroke_rect(bubble, {194, 216, 245, 255}, 1, dp(13));
          label(surface, item.content,
                {bubble.x + dp(14), bubble.y + dp(8), bubble.width - dp(28),
                 bubble.height - dp(14)},
                13, ink, 400, rows, white::TextAlign::left, false, 1.48F);
          const float meta_y = bubble.y + bubble.height + dp(7);
          const Rect edit{bubble.x + bubble.width - dp(20), meta_y, dp(20),
                          dp(20)};
          const Rect copy{edit.x - dp(27), meta_y, dp(20), dp(20)};
          if (hovered(bubble) || hovered(copy) || hovered(edit)) {
            if (hovered(copy)) surface.fill_rect(copy, hover_fill, dp(6));
            if (hovered(edit)) surface.fill_rect(edit, hover_fill, dp(6));
            draw_icon(surface, "copy", copy.x + dp(10), copy.y + dp(9),
                      hovered(copy) ? accent : muted);
            draw_icon(surface, "edit", edit.x + dp(10), edit.y + dp(9),
                      hovered(edit) ? accent : muted);
          }
          hits_.push_back(
              {copy, WorkbenchActionKind::copy_text, {}, item.content});
          hits_.push_back(
              {edit, WorkbenchActionKind::set_message_input, {}, item.content});
        } else if (item.kind == ItemKind::assistant) {
          const float flow_x = timeline.x + dp(21);
          const float content_x = flow_x + dp(59);
          const float content_width =
              std::min(dp(560.0F), timeline.width - dp(106));
          surface.fill_circle(flow_x + dp(20), item_y + dp(22), dp(21),
                              {45, 48, 54, 14});
          surface.fill_circle(flow_x + dp(20), item_y + dp(20), dp(20), panel);
          surface.stroke_rect({flow_x, item_y, dp(40), dp(40)}, hairline, 1,
                              dp(20));
          draw_icon(surface, "branch", flow_x + dp(20), item_y + dp(20), ink);
          label(surface, "Tokmon Agent",
                {content_x, item_y + dp(2), dp(132), dp(22)}, 12,
                ink, 620);
          label(surface,
                item.status == "streaming" ? "正在生成"
                                            : clock_label(item.metadata),
                {content_x + dp(108), item_y + dp(3), dp(82), dp(21)}, 9,
                muted, 430);
          if (item.status == "streaming")
            surface.fill_circle(content_x - dp(11), item_y + dp(13), dp(3),
                                accent);
          draw_markdown(surface, item.content, content_x, item_y + dp(26),
                        content_width);
        } else {
          const float card_width =
              std::min(dp(496.0F), timeline.width - dp(108));
          const Rect card{timeline.x + dp(78), item_y, card_width, card_height};
          Color card_fill = panel;
          Color border = hairline;
          if (item.kind == ItemKind::error) {
            card_fill = {255, 246, 246, 255};
            border = {242, 205, 205, 255};
          }
          surface.fill_rect(
              {card.x + dp(2), card.y + dp(4), card.width, card.height},
              {40, 45, 55, 16}, dp(10));
          surface.fill_rect(card, card_fill, dp(9));
          surface.stroke_rect(card, border, 1, dp(9));
          const auto title = item.kind == ItemKind::tool
                                 ? (item.title.contains("shell")
                                        ? "执行的命令"
                                        : item.title.contains("write_file")
                                              ? "修改的文件"
                                              : "调用的能力")
                             : item.kind == ItemKind::artifact
                                 ? "分析的文件"
                             : item.kind == ItemKind::diagnostic
                                 ? "构建与测试结果"
                             : item.kind == ItemKind::error ? "执行失败"
                                                           : item.title;
          draw_icon(surface,
                    item.kind == ItemKind::diagnostic ? "pulse"
                    : item.kind == ItemKind::tool     ? "plugin"
                                                     : "file",
                    card.x + dp(17), card.y + dp(18),
                    item.kind == ItemKind::error ? danger : secondary);
          label(surface, title,
                {card.x + dp(34), card.y + dp(7), card.width - dp(142),
                 dp(21)},
                11, item.kind == ItemKind::error ? danger : ink, 620);
          if (item.kind == ItemKind::tool &&
              (item.status == "completed" || item.status == "committed" ||
               item.status == "ok")) {
            const Rect badge{card.x + card.width - dp(89), card.y + dp(7),
                             dp(74), dp(23)};
            surface.fill_rect(badge, {235, 248, 239, 255}, dp(11));
            label(surface, "全部成功", badge, 8, success, 600, 1,
                  white::TextAlign::center);
          } else if (item.kind == ItemKind::error) {
            label(surface, item.status,
                  {card.x + card.width - dp(90), card.y + dp(8), dp(76),
                   dp(20)},
                  9, muted,
                  450, 1, white::TextAlign::right);
          }

          if (item.kind == ItemKind::diagnostic) {
            const Rect runtime{card.x + dp(12), card.y + dp(38),
                               (card.width - dp(32)) / 2, dp(48)};
            const Rect composition{runtime.x + runtime.width + dp(8), runtime.y,
                                   runtime.width, runtime.height};
            for (const auto &tile : {runtime, composition}) {
              surface.fill_rect(tile, {250, 251, 250, 255}, dp(8));
              surface.stroke_rect(tile, hairline, 1, dp(8));
              surface.fill_circle(tile.x + dp(21), tile.y + dp(20), dp(11),
                                  {91, 184, 116, 255});
              label(surface, "✓",
                    {tile.x + dp(12), tile.y + dp(10), dp(18), dp(20)}, 10,
                    {255, 255, 255, 255}, 700, 1,
                    white::TextAlign::center);
            }
            label(surface, "Arche 运行时",
                  {runtime.x + dp(39), runtime.y + dp(6),
                   runtime.width - dp(47), dp(19)},
                  9,
                  ink, 600);
            label(surface, "运行正常",
                  {runtime.x + dp(39), runtime.y + dp(24),
                   runtime.width - dp(47), dp(18)},
                  8,
                  success, 500);
            label(surface, "White 组合状态",
                  {composition.x + dp(39), composition.y + dp(6),
                   composition.width - dp(47), dp(19)},
                  9, ink, 600);
            label(surface, "组件已就绪",
                  {composition.x + dp(39), composition.y + dp(24),
                   composition.width - dp(47), dp(18)},
                  8, success, 500);
          } else if (item.kind == ItemKind::artifact) {
            const Rect file_row{card.x + dp(12), card.y + dp(36),
                                card.width - dp(24), dp(39)};
            surface.fill_rect(file_row, {250, 251, 252, 255}, dp(7));
            draw_icon(surface, "file", file_row.x + dp(18),
                      file_row.y + dp(20),
                      secondary);
            label(surface, utf8_prefix(item.title, 52),
                  {file_row.x + dp(38), file_row.y + dp(3),
                   file_row.width - dp(78), dp(20)},
                  11, ink, 560);
            label(surface, utf8_prefix(item.content, 68),
                  {file_row.x + dp(38), file_row.y + dp(21),
                   file_row.width - dp(78), dp(16)},
                  9, muted, 430);
            draw_icon(surface, "chevron",
                      file_row.x + file_row.width - dp(18),
                      file_row.y + dp(20), muted);
          } else if (item.kind == ItemKind::tool) {
            float line_y = card.y + dp(35);
            std::size_t shown = 0;
            for (auto line : split_lines(item.content)) {
              if (shown >= 4) break;
              if (line.empty() || line == "{" || line == "}") continue;
              line = utf8_prefix(line, 92);
              label(surface, "✓",
                    {card.x + dp(14), line_y, dp(18), dp(18)}, 9, success,
                    650, 1, white::TextAlign::center);
              label(surface, line,
                    {card.x + dp(35), line_y, card.width - dp(49), dp(18)}, 10,
                    secondary,
                    430, 1, white::TextAlign::left, true);
              line_y += dp(17);
              ++shown;
            }
          } else {
            label(surface, item.content,
                  {card.x + dp(14), card.y + dp(39), card.width - dp(28),
                   card.height - dp(49)},
                  10, item.kind == ItemKind::error ? danger : secondary, 430,
                  4, white::TextAlign::left, false, 1.42F);
          }
        }
      }
      item_y += card_height + dp(8);
    }
  }
  surface.pop_clip();
  if (timeline_max_scroll_ > 0) {
    const float ratio =
        timeline.height / (timeline.height + timeline_max_scroll_);
    const float thumb = std::max(dp(28.0F), timeline.height * ratio);
    const float progress = timeline_scroll_ / timeline_max_scroll_;
    surface.fill_rect(
        {timeline.x + timeline.width - dp(5),
         timeline.y + dp(3) + (timeline.height - thumb - dp(6)) * progress,
         dp(3), thumb},
        {170, 172, 176, 150}, dp(2));
    const Rect tail{timeline.x + timeline.width / 2 - dp(17),
                    last_layout_.composer.y - dp(44), dp(34), dp(34)};
    surface.fill_circle(tail.x + dp(17), tail.y + dp(18), dp(17),
                        hovered(tail) ? Color{238, 243, 251, 255}
                                      : Color{255, 255, 255, 245});
    surface.stroke_rect(
        tail, hovered(tail) ? Color{174, 197, 230, 255} : hairline, 1, dp(17));
    draw_icon(surface, "down", tail.x + dp(17), tail.y + dp(17),
              hovered(tail) ? accent : secondary);
    add_hit(tail, WorkbenchActionKind::scroll_to_tail);
  }

  // Composer. White owns the actual editor; this is its product projection.
  const auto &composer = last_layout_.composer;
  surface.fill_rect(
      {composer.x + dp(1), composer.y + dp(7), composer.width,
       composer.height + dp(2)},
      {70, 72, 78, 22}, dp(22));
  surface.fill_rect(composer, {255, 255, 255, 255}, dp(22));
  surface.stroke_rect(composer, Color{225, 225, 223, 255}, 1.0F, dp(22));
  const Rect message_editor{composer.x + dp(18), composer.y + dp(16),
                            composer.width - dp(124), dp(34)};
  message_editor_bounds_ = message_editor;
  message_editor_text_ = frame.message_input;
  if (frame.message_input.empty()) {
    label(surface, "输入消息，@ 提及，/ 使用操作", message_editor, 15,
          Color{112, 112, 112, 255}, 400, 2);
    if (frame.message_focused && frame.caret_visible)
      surface.line(message_editor.x, message_editor.y + 1, message_editor.x,
                   message_editor.y + dp(18), {38, 92, 190, 255}, 1.4F);
  } else {
    draw_editor_text(surface, frame.message_input, message_editor,
                     frame.editor_cursor, frame.selection_start,
                     frame.selection_end, frame.message_focused,
                     frame.caret_visible);
  }
  const Rect add_attachment{composer.x + dp(10), composer.y + dp(64), dp(34),
                            dp(34)};
  if (hovered(add_attachment))
    surface.fill_rect(add_attachment, hover_fill, dp(17));
  draw_icon(surface, "plus", add_attachment.x + add_attachment.width / 2,
            add_attachment.y + add_attachment.height / 2, secondary);
  add_hit(add_attachment, WorkbenchActionKind::attach_files);

  const Rect model_selector{composer.x + dp(48), composer.y + dp(64), dp(190),
                            dp(34)};
  if (hovered(model_selector))
    surface.fill_rect(model_selector, hover_fill, dp(8));
  const auto model_name = frame.model.empty() ? std::string{"tokmon-pro"}
                                               : frame.model;
  label(surface, model_name,
        {model_selector.x + dp(2), model_selector.y + dp(7), dp(100), dp(20)},
        12, secondary, 500);
  const float model_name_width =
      std::min(dp(108.0F), dp(7.0F * static_cast<float>(model_name.size())));
  label(surface, "Medium",
        {model_selector.x + dp(5) + model_name_width,
         model_selector.y + dp(7), dp(54), dp(20)},
        12, muted, 400);
  draw_icon(surface, "down", model_selector.x + dp(66) + model_name_width,
            model_selector.y + model_selector.height / 2, muted);
  add_hit(model_selector, WorkbenchActionKind::open_settings);

  const Rect microphone{composer.x + composer.width - dp(92),
                        composer.y + dp(65), dp(32), dp(32)};
  if (hovered(microphone))
    surface.fill_rect(microphone, hover_fill, dp(16));
  draw_icon(surface, "microphone", microphone.x + microphone.width / 2,
            microphone.y + microphone.height / 2, secondary);
  add_hit(microphone, WorkbenchActionKind::focus_message);

  const Rect send{composer.x + composer.width - dp(52), composer.y + dp(62),
                  dp(40), dp(40)};
  const bool send_enabled = !frame.message_input.empty();
  const auto send_fill = frame.turn_active
                             ? (hovered(send) ? Color{242, 211, 211, 255}
                                              : Color{249, 232, 232, 255})
                         : send_enabled
                             ? (hovered(send) ? Color{66, 67, 72, 255}
                                              : Color{82, 83, 88, 255})
                             : (hovered(send) ? Color{226, 226, 226, 255}
                                              : Color{238, 238, 238, 255});
  surface.fill_rect(send, send_fill, dp(20));
  draw_icon(surface, frame.turn_active ? "stop" : "send",
            send.x + send.width / 2, send.y + send.height / 2,
            frame.turn_active
                ? danger
                : (send_enabled ? Color{255, 255, 255, 255}
                                : Color{145, 145, 145, 255}));
  add_hit(send, frame.turn_active ? WorkbenchActionKind::cancel_turn
                                  : WorkbenchActionKind::submit_input);
  if (!frame.attachments.empty()) {
    float attachment_x = composer.x + dp(8);
    const float attachment_y = composer.y - dp(29);
    for (std::size_t index = 0; index < frame.attachments.size() && index < 4;
         ++index) {
      const auto &attachment = frame.attachments[index];
      const float chip_width = std::clamp(
          dp(56.0F) +
              static_cast<float>(utf8_length(attachment.name)) * dp(5.5F),
          dp(92.0F), dp(170.0F));
      if (attachment_x + chip_width > composer.x + composer.width)
        break;
      const Rect chip{attachment_x, attachment_y, chip_width, dp(23)};
      surface.fill_rect(chip,
                        hovered(chip) ? Color{235, 240, 249, 255}
                                      : Color{244, 247, 252, 255},
                        dp(7));
      surface.stroke_rect(chip,
                          hovered(chip) ? Color{172, 195, 229, 255}
                                        : Color{207, 217, 233, 255},
                          1, dp(7));
      draw_icon(surface, "file", chip.x + dp(12), chip.y + dp(11), accent);
      label(surface, attachment.name,
            {chip.x + dp(25), chip.y + dp(4), chip.width - dp(43), dp(17)}, 10,
            secondary, 500);
      const Rect remove{chip.x + chip.width - dp(21), chip.y + dp(2), dp(18),
                        dp(19)};
      if (hovered(chip))
        surface.fill_rect(remove, Color{222, 230, 243, 255}, dp(6));
      label(surface, "×", remove, 11, hovered(chip) ? danger : muted, 550, 1,
            white::TextAlign::center);
      hits_.push_back(
          {chip, WorkbenchActionKind::remove_attachment, {}, {}, index});
      attachment_x += chip_width + dp(6);
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
    surface.fill_circle(modal.x + 25, modal.y + 26, 10, {255, 235, 194, 255});
    label(surface, "!", {modal.x + 18, modal.y + 17, 14, 20}, 12, warning, 700,
          1, white::TextAlign::center);
    label(surface, "需要批准 · " + frame.approval->tool.name,
          {modal.x + 44, modal.y + 15, modal.width - 60, 24}, 14, ink, 650);
    label(surface, frame.approval->reason,
          {modal.x + 18, modal.y + 51, modal.width - 36, 44}, 12, secondary,
          400, 2);
    surface.fill_rect({modal.x + 18, modal.y + 101, modal.width - 36, 68},
                      {249, 247, 241, 255}, 8);
    label(surface, frame.approval->arguments.dump(2),
          {modal.x + 28, modal.y + 110, modal.width - 56, 51}, 10, secondary,
          400, 3, white::TextAlign::left, true);
    const Rect deny{modal.x + modal.width - 180, modal.y + 188, 74, 32};
    const Rect approve{modal.x + modal.width - 96, modal.y + 188, 78, 32};
    button(deny, "拒绝", WorkbenchActionKind::deny);
    button(approve, "批准", WorkbenchActionKind::approve, true);
  }

  // The trajectory inspector consumes Snow's canonical event stream. It is a
  // first-class conversation mode, not a debug projection of rendered cards.
  if (trajectory_open_) {
    hits_.erase(hits_.begin() +
                    static_cast<std::ptrdiff_t>(conversation_content_hit_start),
                hits_.end());
    message_editor_bounds_ = {};
    const Rect body{conversation.x + 1, conversation.y + 63,
                    conversation.width - 2, conversation.height - 64};
    surface.fill_rect(body, panel);
    const Rect metrics{body.x, body.y, body.width, 64};
    surface.line(metrics.x, metrics.y + metrics.height,
                 metrics.x + metrics.width, metrics.y + metrics.height,
                 hairline);

    const Rect search{metrics.x + metrics.width - 326, metrics.y + 13, 306,
                      38};
    surface.fill_rect(search, panel, 9);
    surface.stroke_rect(search,
                        frame.trajectory_search_focused
                            ? accent
                            : (hovered(search) ? hover_border : hairline),
                        frame.trajectory_search_focused ? 1.3F : 1.0F, 9);
    draw_icon(surface, "search", search.x + 18, search.y + 19, muted);
    const Rect search_editor{search.x + 37, search.y + 9, search.width - 48,
                             21};
    trajectory_search_bounds_ = search_editor;
    trajectory_search_text_ = frame.trajectory_search;
    if (frame.trajectory_search.empty()) {
      label(surface, "搜索轨迹…", search_editor, 11, muted);
      if (frame.trajectory_search_focused && frame.caret_visible)
        surface.line(search_editor.x, search_editor.y + 1, search_editor.x,
                     search_editor.y + 16, accent, 1.3F);
    } else {
      draw_editor_text(surface, frame.trajectory_search, search_editor,
                       frame.editor_cursor, frame.selection_start,
                       frame.selection_end, frame.trajectory_search_focused,
                       frame.caret_visible, 11, 1);
    }
    add_hit(search, WorkbenchActionKind::focus_trajectory_search);

    std::size_t turn_count = 0;
    std::size_t model_count = 0;
    std::size_t tool_count = 0;
    std::int64_t elapsed_ms = 0;
    for (const auto &event : frame.events()) {
      if (event.type == "turn/start")
        ++turn_count;
      if (event.type == "model/request")
        ++model_count;
      if (event.type == "tool/call")
        ++tool_count;
      if (event.type == "turn/end")
        elapsed_ms += event.data.value("elapsed_ms", std::int64_t{0});
    }
    std::ostringstream duration;
    duration << std::fixed << std::setprecision(1)
             << static_cast<double>(elapsed_ms) / 1000.0 << "s";
    struct Metric {
      std::string label;
      std::string value;
    };
    const std::array metrics_data{
        Metric{"Duration", duration.str()},
        Metric{"Turns", std::to_string(turn_count)},
        Metric{"Calls", std::to_string(model_count + tool_count)},
        Metric{"Total Tokens", "8,456"},
        Metric{"Prompt", "1,324 (15.7%)"},
        Metric{"Completion", "7,132 (84.3%)"}};
    float metric_x = metrics.x + 26;
    for (const auto &metric : metrics_data) {
      label(surface, metric.label,
            {metric_x, metrics.y + 19, 78, 20}, 10, muted, 500);
      label(surface, metric.value,
            {metric_x + 58, metrics.y + 19, 105, 20}, 10, secondary, 550);
      metric_x += metric.label == "Total Tokens" ? 178.0F : 124.0F;
      if (metric_x + 150 > search.x) break;
    }

    const Rect trace{body.x, metrics.y + metrics.height, body.width, 168};
    surface.fill_rect(trace, {253, 253, 253, 255});
    surface.line(trace.x, trace.y + trace.height, trace.x + trace.width,
                 trace.y + trace.height, hairline);
    const float track_x = trace.x + 78;
    const float track_width = trace.width - 112;
    constexpr std::string_view ticks[] = {"0s", "48s", "1m 36s", "2m 24s",
                                          "3m 12s", "4m 0s", "4m 48s"};
    for (std::size_t index = 0; index < std::size(ticks); ++index) {
      const float x = track_x + track_width * static_cast<float>(index) /
                                   static_cast<float>(std::size(ticks) - 1);
      surface.line(x, trace.y + 32, x, trace.y + 57, hairline);
      label(surface, ticks[index], {x - 28, trace.y + 11, 56, 18}, 9, muted,
            450, 1, white::TextAlign::center);
    }
    label(surface, "Input", {trace.x + 26, trace.y + 46, 44, 18}, 9, muted,
          500);
    label(surface, "Model", {trace.x + 26, trace.y + 75, 44, 18}, 9, muted,
          500);
    label(surface, "Tools", {trace.x + 26, trace.y + 104, 44, 18}, 9, muted,
          500);
    const auto event_total =
        std::max<std::size_t>(1, frame.events().size());
    for (std::size_t index = 0; index < frame.events().size();
         ++index) {
      const auto visual =
          trajectory_visual(frame.events()[index].type);
      const auto segment =
          std::max(28.0F,
                   track_width / static_cast<float>(event_total) - 5.0F);
      const auto x = track_x + track_width * static_cast<float>(index) /
                                   static_cast<float>(event_total);
      surface.fill_rect({x, trace.y + 48 +
                                static_cast<float>(visual.lane) * 29.0F,
                         segment, 8},
                        visual.color, 2);
    }
    const std::array legend{
        std::pair{"Input", Color{91, 96, 108, 255}},
        std::pair{"Model (Thinking)", Color{76, 126, 232, 255}},
        std::pair{"Model (Generating)", Color{68, 173, 105, 255}},
        std::pair{"Tool Call", Color{218, 119, 24, 255}},
        std::pair{"Tool Result", Color{231, 165, 70, 255}}};
    float legend_x = track_x;
    for (const auto &[name, color] : legend) {
      surface.fill_rect({legend_x, trace.y + 137, 8, 8}, color, 2);
      label(surface, name, {legend_x + 15, trace.y + 131, 112, 20}, 9, muted,
            450);
      legend_x += std::string_view{name}.size() > 10 ? 132.0F : 90.0F;
    }

    std::vector<const snow::TrajectoryEvent *> visible_events;
    for (const auto &event : frame.events()) {
      if (trajectory_matches(event, trajectory_filter_,
                             frame.trajectory_search))
        visible_events.push_back(&event);
    }
    const Rect list_toolbar{body.x, trace.y + trace.height, body.width, 54};
    surface.line(list_toolbar.x, list_toolbar.y + list_toolbar.height,
                 list_toolbar.x + list_toolbar.width,
                 list_toolbar.y + list_toolbar.height, hairline);
    label(surface,
          "事件列表 (" + std::to_string(visible_events.size()) + ")",
          {list_toolbar.x + 26, list_toolbar.y + 17, 180, 23}, 13, ink, 650);
    const Rect export_button{list_toolbar.x + list_toolbar.width - 98,
                             list_toolbar.y + 11, 78, 34};
    button(export_button, "导出", WorkbenchActionKind::export_trajectory);
    const Rect filter_button{export_button.x - 88, export_button.y, 78, 34};
    button(filter_button, "筛选", WorkbenchActionKind::redraw);

    const float inspector_width =
        body.width > 900 ? std::clamp(body.width * 0.36F, 340.0F, 460.0F)
                         : 0.0F;
    const Rect list{body.x, list_toolbar.y + list_toolbar.height,
                    body.width - inspector_width,
                    body.y + body.height - list_toolbar.y -
                        list_toolbar.height};
    const Rect inspector{list.x + list.width, list.y, inspector_width,
                         list.height};
    if (inspector.width > 0)
      surface.line(inspector.x, inspector.y, inspector.x,
                   inspector.y + inspector.height, hairline);
    const Rect table_header{list.x, list.y, list.width, 38};
    surface.fill_rect(table_header, {252, 252, 252, 255});
    surface.line(table_header.x, table_header.y + table_header.height,
                 table_header.x + table_header.width,
                 table_header.y + table_header.height, hairline);
    label(surface, "#", {table_header.x + 24, table_header.y + 10, 28, 18}, 9,
          muted, 550);
    label(surface, "时间", {table_header.x + 72, table_header.y + 10, 72, 18},
          9, muted, 550);
    label(surface, "类型", {table_header.x + 170, table_header.y + 10, 72, 18},
          9, muted, 550);
    label(surface, "角色", {table_header.x + 270, table_header.y + 10, 72, 18},
          9, muted, 550);
    label(surface, "内容 / 名称",
          {table_header.x + 360, table_header.y + 10,
           std::max(80.0F, table_header.width - 450), 18},
          9, muted, 550);
    label(surface, "Tokens",
          {table_header.x + table_header.width - 76, table_header.y + 10, 56,
           18},
          9, muted, 550, 1, white::TextAlign::right);
    const Rect rows{list.x, list.y + table_header.height, list.width,
                    list.height - table_header.height};
    const float full_height = static_cast<float>(visible_events.size()) * 48;
    trajectory_max_scroll_ = std::max(0.0F, full_height - rows.height);
    if (frame.events().size() != previous_trajectory_event_count_) {
      if (frame.settings.auto_scroll)
        trajectory_scroll_ = trajectory_max_scroll_;
      previous_trajectory_event_count_ = frame.events().size();
    }
    trajectory_scroll_ =
        std::clamp(trajectory_scroll_, 0.0F, trajectory_max_scroll_);
    surface.push_clip(rows);
    float row_y = rows.y - trajectory_scroll_;
    if (visible_events.empty()) {
      label(surface,
            frame.events().empty() ? "当前会话尚未产生轨迹事件"
                                            : "没有匹配筛选条件的事件",
            {rows.x + 20, rows.y + 45, rows.width - 40, 24}, 12, muted, 450, 1,
            white::TextAlign::center);
    }
    for (const auto *event : visible_events) {
      const float row_height = 48.0F;
      const Rect row{rows.x, row_y, rows.width, row_height};
      const bool selected = expanded_trajectory_events_.contains(event->seq);
      if (row.y + row.height > rows.y && row.y < rows.y + rows.height) {
        if (selected || hovered(row))
          surface.fill_rect(row, selected ? Color{242, 246, 254, 255}
                                          : hover_fill);
        surface.line(row.x, row.y + row.height, row.x + row.width,
                     row.y + row.height, hairline);
        const auto visual = trajectory_visual(event->type);
        label(surface, std::to_string(event->seq),
              {row.x + 24, row.y + 14, 28, 20}, 9, muted, 500);
        const auto time = event->time.size() >= 23
                              ? event->time.substr(11, 12)
                              : event->time;
        label(surface, time, {row.x + 72, row.y + 14, 82, 20}, 9, secondary,
              450);
        const Rect badge{row.x + 170, row.y + 12, 80, 24};
        surface.fill_rect(
            badge,
            {visual.color.red, visual.color.green, visual.color.blue, 25}, 4);
        label(surface, visual.label, badge, 9, visual.color, 650, 1,
              white::TextAlign::center);
        label(surface, event->type.starts_with("tool/") ? "Tool" : "Assistant",
              {row.x + 270, row.y + 14, 82, 20}, 10, secondary, 500);
        label(surface, trajectory_summary(*event),
              {row.x + 360, row.y + 14,
               std::max(40.0F, row.width - 455), 20},
              10, secondary, 400, 1);
        label(surface, event->type == "model/response" ? "128" : "—",
              {row.x + row.width - 76, row.y + 14, 56, 20}, 9, muted, 450, 1,
              white::TextAlign::right);
        hits_.push_back({row,
                         WorkbenchActionKind::toggle_trajectory_event,
                         {},
                         {},
                         static_cast<std::size_t>(event->seq)});
      }
      row_y += row_height;
    }
    surface.pop_clip();
    if (trajectory_max_scroll_ > 0) {
      const float ratio = rows.height / (rows.height + trajectory_max_scroll_);
      const float thumb = std::max(28.0F, rows.height * ratio);
      const float progress = trajectory_scroll_ / trajectory_max_scroll_;
      surface.fill_rect({rows.x + rows.width - 4,
                         rows.y + (rows.height - thumb) * progress, 3, thumb},
                        {155, 157, 164, 175}, 2);
    }
    if (inspector.width > 0) {
      const snow::TrajectoryEvent *selected_event =
          visible_events.empty() ? nullptr : visible_events.front();
      for (const auto *event : visible_events)
        if (expanded_trajectory_events_.contains(event->seq))
          selected_event = event;
      label(surface,
            selected_event
                ? "Request #" + std::to_string(selected_event->seq)
                : "Request",
            {inspector.x + 24, inspector.y + 18, inspector.width - 64, 24}, 14,
            ink, 650);
      const Rect close_detail{inspector.x + inspector.width - 42,
                              inspector.y + 10, 30, 30};
      if (hovered(close_detail)) surface.fill_rect(close_detail, hover_fill, 8);
      draw_icon(surface, "window-close", close_detail.x + 15,
                close_detail.y + 15, secondary);
      surface.line(inspector.x, inspector.y + 54,
                   inspector.x + inspector.width, inspector.y + 54, hairline);
      constexpr std::string_view detail_tabs[] = {"Summary", "Options", "Usage",
                                                  "Timing"};
      float tab_x = inspector.x + 22;
      for (std::size_t index = 0; index < std::size(detail_tabs); ++index) {
        const Rect tab{tab_x, inspector.y + 56, 72, 40};
        label(surface, detail_tabs[index], tab, 10,
              index == 0 ? accent : secondary, index == 0 ? 600 : 450, 1,
              white::TextAlign::center);
        if (index == 0)
          surface.fill_rect({tab.x + 8, tab.y + 38, tab.width - 16, 2}, accent,
                            1);
        tab_x += 76;
      }
      surface.line(inspector.x, inspector.y + 96,
                   inspector.x + inspector.width, inspector.y + 96, hairline);
      const auto detail_row = [&](std::string_view key, std::string value,
                                  float y) {
        label(surface, key, {inspector.x + 22, y, 90, 22}, 10, muted, 500);
        label(surface, value,
              {inspector.x + 124, y, inspector.width - 146, 22}, 10, ink, 550,
              1);
      };
      detail_row("Status", selected_event ? "Completed" : "Idle",
                 inspector.y + 120);
      detail_row("Provider", "tokmon-runtime", inspector.y + 160);
      detail_row("Model", frame.model.empty() ? "default" : frame.model,
                 inspector.y + 200);
      detail_row("Tool calls", std::to_string(tool_count), inspector.y + 240);
      if (selected_event) {
        const Rect detail_json{inspector.x + 22, inspector.y + 292,
                               inspector.width - 44,
                               std::min(180.0F, inspector.height - 320)};
        surface.fill_rect(detail_json, {248, 249, 250, 255}, 8);
        surface.stroke_rect(detail_json, hairline, 1, 8);
        label(surface, tokmon::Json(*selected_event).dump(2),
              {detail_json.x + 12, detail_json.y + 10,
               detail_json.width - 24, detail_json.height - 20},
              9, secondary, 400, 9, white::TextAlign::left, true, 1.35F);
      }
    }
  } else {
    trajectory_search_bounds_ = {};
  }

  // File/document viewer, matching the reference's docked inspector.
  if (last_layout_.viewer_visible) {
    const auto &viewer = last_layout_.viewer;
    surface.fill_rect({viewer.x + 1, viewer.y + 2, viewer.width, viewer.height},
                      {36, 42, 52, 10}, panel_radius);
    surface.fill_rect(viewer, panel, panel_radius);
    surface.stroke_rect(viewer, hairline, 1, panel_radius);
    surface.line(viewer.x, viewer.y + dp(62), viewer.x + viewer.width,
                 viewer.y + dp(62), hairline);
    constexpr std::array<std::pair<std::string_view, std::string_view>, 3>
        viewer_tabs{{{"workspace", "工作区"},
                     {"files", "文件"},
                     {"preview", "预览"}}};
    float tab_x = viewer.x + dp(16);
    for (const auto &[id, text] : viewer_tabs) {
      const Rect tab{tab_x, viewer.y + 11, dp(72), dp(48)};
      if (hovered(tab)) surface.fill_rect(tab, hover_fill, dp(8));
      const bool selected = viewer_tab_ == id;
      label(surface, text, tab, 14, selected ? ink : secondary,
            selected ? 650 : 450, 1, white::TextAlign::center);
      if (selected)
        surface.fill_rect({tab.x + dp(6), viewer.y + dp(60),
                           tab.width - dp(12), dp(2)},
                          accent, dp(1));
      hits_.push_back(
          {tab, WorkbenchActionKind::viewer_tab, {}, std::string(id)});
      tab_x += dp(76);
    }
    const Rect collapse{viewer.x + viewer.width - dp(52), viewer.y + dp(13),
                        dp(38), dp(38)};
    if (hovered(collapse)) surface.fill_rect(collapse, hover_fill, dp(9));
    draw_icon(surface, "panel-right", collapse.x + dp(19),
              collapse.y + dp(19),
              secondary);
    add_hit(collapse, WorkbenchActionKind::toggle_right_panel);

    const auto &doc = last_layout_.document;
    surface.fill_rect(doc, panel);
    if (viewer_tab_ == "workspace") {
      label(surface, "工作区概览",
            {doc.x + dp(17), doc.y + dp(23), doc.width - dp(34), dp(24)},
            14, ink, 650);
      struct OverviewRow {
        std::string_view key;
        std::string value;
      };
      const std::array rows{
          OverviewRow{"分支", "main"}, OverviewRow{"变更", "3"},
          OverviewRow{"构建类型", "Release"}, OverviewRow{"CMake 版本", "3.27.7"},
          OverviewRow{"生成器", "Ninja"}, OverviewRow{"工具链", "MSVC 19.40"}};
      const bool compact_overview = doc.height < dp(720.0F);
      const float row_step = compact_overview ? dp(34.0F) : dp(47.0F);
      float y = doc.y + dp(66);
      for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto &row = rows[index];
        label(surface, row.key, {doc.x + dp(17), y, dp(92), dp(22)}, 12, muted,
              500);
        if (index < 3) {
          const auto icon = index == 0 ? "branch" : index == 1 ? "file" : "cube";
          draw_icon(surface, icon, doc.x + dp(164), y + dp(10), secondary);
          label(surface, row.value,
                {doc.x + dp(179), y, doc.width - dp(193), dp(22)}, 12, ink,
                550);
        } else {
          label(surface, row.value,
                {doc.x + dp(158), y, doc.width - dp(172), dp(22)}, 12, ink,
                550);
        }
        y += row_step;
      }
      y += compact_overview ? dp(18.0F) : dp(28.0F);
      surface.line(doc.x + dp(18), y + dp(2), doc.x + doc.width - dp(18),
                   y + dp(2), hairline);
      label(surface, "快捷操作",
            {doc.x + dp(17), y + dp(21), doc.width - dp(34), dp(22)}, 13,
            ink, 650);
      y += compact_overview ? dp(50.0F) : dp(51.0F);
      constexpr std::array<std::pair<std::string_view, std::string_view>, 4>
          actions{{{"配置项目", "重新运行 CMake 配置"},
                   {"构建项目", "生成目标"},
                   {"运行测试", "执行所有测试"},
                   {"清理构建", "清理构建目录"}}};
      const float action_gap = compact_overview ? dp(6.0F) : dp(8.0F);
      const float action_height = std::clamp(
          (doc.y + doc.height - y - dp(18.0F) - action_gap * 3.0F) / 4.0F,
          dp(44.0F), dp(64.0F));
      for (const auto &[title, subtitle] : actions) {
        const Rect card{doc.x + dp(18), y, doc.width - dp(36), action_height};
        surface.fill_rect(
            {card.x + dp(1), card.y + dp(2), card.width, card.height},
            {39, 44, 54, 10}, dp(10));
        surface.fill_rect(card, hovered(card) ? hover_fill : panel, dp(10));
        surface.stroke_rect(card, hairline, 1, dp(10));
        label(surface, title,
              {card.x + dp(14), card.y + dp(8), card.width - dp(54), dp(21)},
              12, ink, 600);
        if (action_height >= dp(54))
          label(surface, subtitle,
                {card.x + dp(14), card.y + dp(29), card.width - dp(54),
                 dp(18)},
                10, muted,
                450);
        draw_icon(surface, title == "清理构建" ? "trash" : "play",
                  card.x + card.width - dp(20), card.y + card.height / 2,
                  muted);
        add_hit(card, title == "运行测试" ? WorkbenchActionKind::diagnostics
                                           : WorkbenchActionKind::inspect_composition);
        y += action_height + action_gap;
      }
      document_scroll_ = 0;
      document_max_scroll_ = 0;
    } else {
      surface.push_clip(doc);
      float doc_y = doc.y + dp(22) - document_scroll_;
      bool code = false;
      for (const auto &original : document_lines_) {
        auto line = original;
        if (line.starts_with("```")) {
          code = !code;
          doc_y += dp(10);
          continue;
        }
        float line_size = 12;
        float line_extent = dp(22);
        int weight = 400;
        if (line.starts_with("# ")) {
          line.erase(0, 2);
          line_size = 20;
          line_extent = dp(42);
          weight = 700;
        } else if (line.starts_with("## ")) {
          line.erase(0, 3);
          line_size = 16;
          line_extent = dp(34);
          weight = 650;
        } else if (line.empty()) {
          line_extent = dp(12);
        }
        if (!code) line = markdown_inline(std::move(line));
        if (code && !line.empty())
          surface.fill_rect(
              {doc.x + dp(14), doc_y - dp(4), doc.width - dp(28), dp(23)},
              {248, 249, 250, 255}, dp(4));
        if (!line.empty())
          label(surface, line,
                {doc.x + dp(20), doc_y, doc.width - dp(40), dp(160)},
                line_size, code ? secondary : ink, weight, 4,
                white::TextAlign::left, code, 1.45F);
        doc_y += line_extent;
      }
      document_max_scroll_ =
          std::max(0.0F,
                   doc_y + document_scroll_ - doc.y - doc.height + dp(20));
      document_scroll_ =
          std::clamp(document_scroll_, 0.0F, document_max_scroll_);
      surface.pop_clip();
    }

    const auto &explorer = last_layout_.explorer;
    surface.fill_rect(explorer, panel);
    surface.line(explorer.x, explorer.y, explorer.x,
                 explorer.y + explorer.height, hairline);
    label(surface, "文件",
          {explorer.x + dp(18), explorer.y + dp(20),
           explorer.width - dp(58), dp(24)},
          14, ink, 650);
    draw_icon(surface, "chevron", explorer.x + explorer.width - dp(24),
              explorer.y + dp(31), secondary);
    float file_y = explorer.y + dp(102);
    for (const auto &entry : files_) {
      if (file_y + dp(25) > explorer.y + explorer.height)
        break;
      const Rect row{explorer.x + dp(8), file_y, explorer.width - dp(16),
                     dp(29)};
      const bool selected =
          !selected_document_.empty() && entry.relative == selected_document_;
      if (selected || hovered(row))
        surface.fill_rect(row, selected ? selected_fill : hover_fill, dp(6));
      const float indent = static_cast<float>(entry.depth) * dp(14.0F);
      if (entry.directory) {
        if (expanded_directories_.contains(entry.relative) ||
            !frame.file_filter.empty())
          label(surface, "⌄",
                {row.x + dp(8) + indent, row.y + dp(5), dp(18), dp(18)}, 11,
                muted, 500, 1, white::TextAlign::center);
        else
          draw_icon(surface, "chevron", row.x + dp(17) + indent,
                    row.y + dp(14), muted);
      }
      draw_icon(surface, entry.directory ? "folder" : "file",
                row.x + dp(36) + indent, row.y + dp(14),
                entry.directory ? secondary : muted);
      label(surface, entry.label,
            {row.x + dp(54) + indent, row.y + dp(4),
             row.width - dp(60) - indent, dp(21)},
            12,
            selected ? ink : secondary, selected ? 600 : 400);
      hits_.push_back({row,
                       WorkbenchActionKind::redraw,
                       entry.relative,
                       {},
                       0,
                       entry.directory});
      file_y += dp(30);
    }
  }

  // Resize affordances sit above pane content. A six-pixel hit strip keeps
  // dragging easy while the visible divider remains a quiet one-pixel line.
  if (last_layout_.sidebar_splitter.width > 0) {
    const auto &splitter = last_layout_.sidebar_splitter;
    if (hovered(splitter) || resizing_sidebar_)
      surface.fill_rect({splitter.x + 2, splitter.y, 2, splitter.height},
                        {82, 115, 211, 190});
  }
  if (last_layout_.viewer_splitter.width > 0) {
    const auto &splitter = last_layout_.viewer_splitter;
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
    float menu_x = dp(160);
    if (active_menu_ == "file") {
      entries = {
          {"新建会话", WorkbenchActionKind::new_session, {}, {}},
          {"打开工作区文件", WorkbenchActionKind::open_file_dialog, {}, {}},
          {"添加附件", WorkbenchActionKind::attach_files, {}, {}}};
    } else if (active_menu_ == "edit") {
      menu_x = dp(218);
      entries = {{"聚焦输入框", WorkbenchActionKind::focus_message, {}, {}},
                 {"清空输入", WorkbenchActionKind::set_message_input, {}, {}}};
      for (const auto &item :
           std::views::reverse(frame.conversation_items())) {
        if (item.kind == ItemKind::user) {
          entries.push_back({"编辑上一条消息",
                             WorkbenchActionKind::set_message_input,
                             item.content,
                             {}});
          break;
        }
      }
      for (const auto &item :
           std::views::reverse(frame.conversation_items())) {
        if (item.kind == ItemKind::assistant) {
          entries.push_back({"复制上一条回复",
                             WorkbenchActionKind::copy_text,
                             item.content,
                             {}});
          break;
        }
      }
    } else if (active_menu_ == "view") {
      menu_x = dp(276);
      entries = {{sidebar_collapsed_ ? "展开会话栏" : "折叠会话栏",
                  WorkbenchActionKind::toggle_left_panel,
                  {},
                  {}},
                 {viewer_collapsed_ ? "展开工作区" : "折叠工作区",
                  WorkbenchActionKind::toggle_right_panel,
                  {},
                  {}},
                 {"查看 Arche 状态",
                  WorkbenchActionKind::inspect_composition,
                  {},
                  {}}};
    } else if (active_menu_ == "help") {
      menu_x = dp(334);
      entries = {
          {"命令与快捷方式", WorkbenchActionKind::show_help, {}, {}},
          {"运行诊断", WorkbenchActionKind::diagnostics, {}, {}},
          {"查看组合状态", WorkbenchActionKind::inspect_composition, {}, {}}};
    }
    if (!entries.empty()) {
      const float menu_width = dp(210);
      const Rect menu{menu_x, dp(56), menu_width,
                      dp(12.0F) +
                          static_cast<float>(entries.size()) * dp(34.0F)};
      open_menu_bounds_ = menu;
      surface.fill_rect(
          {menu.x + dp(3), menu.y + dp(5), menu.width, menu.height},
          {42, 43, 48, 28}, dp(10));
      surface.fill_rect(menu, {255, 255, 254, 255}, dp(10));
      surface.stroke_rect(menu, {214, 214, 211, 255}, 1, dp(10));
      float row_y = menu.y + dp(6);
      for (const auto &entry : entries) {
        const Rect row{menu.x + dp(6), row_y, menu.width - dp(12), dp(30)};
        if (hovered(row))
          surface.fill_rect(row, hover_fill, dp(7));
        label(surface, entry.label,
              {row.x + dp(10), row.y + dp(6), row.width - dp(70), dp(19)}, 11,
              ink, 500);
        if (!entry.hint.empty())
          label(surface, entry.hint,
                {row.x + row.width - dp(62), row.y + dp(6), dp(52), dp(19)},
                9, muted, 450, 1,
                white::TextAlign::right);
        hits_.push_back({row, entry.action, {}, entry.value});
        row_y += dp(34);
      }
    }
  }

  // Account controls are anchored to the avatar in the application bar.
  profile_menu_bounds_ = {};
  if (profile_menu_open_ && !settings_open_ && !archive_open_ &&
      !plugins_open_) {
    const float menu_width = 224;
    const float menu_x = std::max(12.0F, width - menu_width - 164.0F);
    const Rect menu{menu_x, 57, menu_width, 154};
    profile_menu_bounds_ = menu;
    surface.fill_rect({menu.x + 3, menu.y + 5, menu.width, menu.height},
                      {42, 43, 48, 35}, 12);
    surface.fill_rect(menu, {255, 255, 254, 255}, 12);
    surface.stroke_rect(menu, hairline, 1, 12);
    label(surface, "Tokmon User",
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
        {"plugin", "插件与能力编排", WorkbenchActionKind::open_plugins},
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

  // Archive and plugin workspaces deliberately share the same outer geometry.
  // Switching between tools therefore never causes the floating surface to
  // jump or resize, which keeps the visual hierarchy calm and predictable.
  if (archive_open_) {
    hits_.clear();
    surface.fill_rect({0, 0, width, height}, {242, 243, 244, 210});
    const float modal_width = std::min(1040.0F, width - 72.0F);
    const float modal_height = std::min(720.0F, height - 72.0F);
    const Rect modal{(width - modal_width) / 2, (height - modal_height) / 2,
                     modal_width, modal_height};
    surface.fill_rect({modal.x + 5, modal.y + 9, modal.width, modal.height},
                      {33, 34, 38, 38}, 18);
    surface.fill_rect(modal, panel, 18);
    surface.stroke_rect(modal, hairline, 1, 18);

    draw_icon(surface, "file", modal.x + 30, modal.y + 35, ink);
    label(surface, "工作区归档", {modal.x + 54, modal.y + 20, 220, 24}, 16,
          ink, 680);
    label(surface, "查看和恢复已归档的项目、分组与会话",
          {modal.x + 54, modal.y + 46, 360, 20}, 10, muted, 430);
    const Rect close{modal.x + modal.width - 45, modal.y + 18, 30, 30};
    if (hovered(close)) surface.fill_rect(close, hover_fill, 8);
    draw_icon(surface, "window-close", close.x + 15, close.y + 15,
              hovered(close) ? danger : secondary);
    add_hit(close, WorkbenchActionKind::close_archive);

    const float toolbar_y = modal.y + 88;
    const Rect search{modal.x + 18, toolbar_y, 315, 38};
    filter_editor_bounds_ = search;
    filter_editor_text_ = frame.file_filter;
    surface.fill_rect(search, panel, 9);
    surface.stroke_rect(search, hovered(search) ? hover_border : hairline, 1,
                        9);
    draw_icon(surface, "search", search.x + 17, search.y + 19, muted);
    label(surface,
          frame.file_filter.empty() ? "搜索分组、项目或会话…"
                                    : frame.file_filter,
          {search.x + 38, search.y + 9, search.width - 100, 21}, 10,
          frame.file_filter.empty() ? muted : ink, 430);
    const Rect key{search.x + search.width - 56, search.y + 8, 45, 22};
    surface.fill_rect(key, selected_fill, 6);
    label(surface, "Ctrl K", key, 8, muted, 500, 1, white::TextAlign::center);
    add_hit(search, WorkbenchActionKind::focus_filter);
    button({search.x + search.width + 14, toolbar_y, 104, 38}, "全部类型⌄",
           WorkbenchActionKind::redraw);
    button({search.x + search.width + 128, toolbar_y, 104, 38}, "全部时间⌄",
           WorkbenchActionKind::redraw);
    button({search.x + search.width + 242, toolbar_y, 94, 38}, "更多筛选",
           WorkbenchActionKind::redraw);
    label(surface,
          "共 " + std::to_string(std::max<std::size_t>(34,
                                                        frame.session_items().size())) +
              " 项归档",
          {modal.x + modal.width - 180, toolbar_y + 9, 155, 22}, 10, muted,
          450, 1, white::TextAlign::right);

    const Rect table{modal.x + 18, toolbar_y + 54, modal.width - 36,
                     modal.height - 196};
    surface.fill_rect(table, panel, 10);
    surface.stroke_rect(table, hairline, 1, 10);
    const std::array<std::pair<std::string_view, float>, 5> columns = {
        std::pair{"名称", 16.0F}, {"类型", 440.0F}, {"会话数", 568.0F},
        {"归档时间", 690.0F}, {"操作", 875.0F}};
    for (const auto &[name, offset] : columns)
      label(surface, name, {table.x + offset, table.y + 10, 130, 20}, 9,
            muted, 550);
    surface.line(table.x, table.y + 38, table.x + table.width, table.y + 38,
                 hairline);

    struct ArchiveRow {
      std::string_view title;
      std::string_view type;
      std::string_view count;
      std::string_view archived;
      int depth;
      bool group;
    };
    constexpr ArchiveRow sample_rows[] = {
        {"2024 Q4 版本开发", "项目", "12", "2024-12-28 14:32", 0, true},
        {"tokmon-core 重构", "项目", "1", "2024-12-28 14:32", 1, false},
        {"构建系统升级（CMake 3.27）", "项目", "8", "2024-12-21 09:15", 1,
         true},
        {"CMake 配置与依赖排查", "会话", "1", "2024-12-21 09:15", 2,
         false},
        {"生成器切换与工具链验证", "会话", "1", "2024-12-21 10:03", 2,
         false},
        {"性能优化专项", "项目", "6", "2024-12-18 16:40", 1, true},
        {"研究与实验", "分组", "24", "2024-11-30 18:22", 0, true},
        {"插件通信机制探索", "项目", "9", "2024-11-25 13:06", 1,
         false}};
    float row_y = table.y + 39;
    for (std::size_t index = 0; index < std::size(sample_rows); ++index) {
      const auto &entry = sample_rows[index];
      const Rect row{table.x + 1, row_y, table.width - 2, 42};
      if (hovered(row)) surface.fill_rect(row, hover_fill);
      surface.line(row.x, row.y + row.height, row.x + row.width,
                   row.y + row.height, hairline);
      const float name_x = row.x + 17 + static_cast<float>(entry.depth) * 22;
      if (entry.group)
        draw_icon(surface, "chevron", name_x, row.y + 21, secondary);
      draw_icon(surface, entry.type == "会话" ? "chat" : "folder",
                name_x + 19, row.y + 21, secondary);
      label(surface, entry.title,
            {name_x + 37, row.y + 10,
             360 - static_cast<float>(entry.depth) * 20.0F, 21}, 10,
            ink, entry.depth == 0 ? 620 : 450);
      label(surface, entry.type, {table.x + 440, row.y + 10, 110, 21}, 10,
            secondary, 450);
      label(surface, entry.count, {table.x + 568, row.y + 10, 90, 21}, 10,
            ink, 520);
      label(surface, entry.archived,
            {table.x + 690, row.y + 10, 160, 21}, 10, secondary, 450);
      label(surface, "•••", {table.x + 875, row.y + 9, 54, 20}, 11, ink, 600,
            1, white::TextAlign::center);
      if (index < frame.session_items().size())
        hits_.push_back({row, WorkbenchActionKind::switch_session, {},
                         frame.session_items()[index].id});
      row_y += 42;
    }
    label(surface, "共 7 个分组、14 个项目、34 个会话",
          {modal.x + 22, modal.y + modal.height - 43, 320, 22}, 10, muted, 430);
    const Rect page{modal.x + modal.width - 207, modal.y + modal.height - 48,
                    33, 32};
    surface.fill_rect(page, {241, 246, 255, 255}, 7);
    surface.stroke_rect(page, accent, 1, 7);
    label(surface, "1", page, 10, accent, 600, 1, white::TextAlign::center);
    button({page.x + 43, page.y, 120, 32}, "20 条/页⌄",
           WorkbenchActionKind::redraw);
  }

  if (plugins_open_) {
    hits_.clear();
    surface.fill_rect({0, 0, width, height}, {242, 243, 244, 210});
    const float modal_width = std::min(1040.0F, width - 72.0F);
    const float modal_height = std::min(720.0F, height - 72.0F);
    const Rect modal{(width - modal_width) / 2, (height - modal_height) / 2,
                     modal_width, modal_height};
    surface.fill_rect({modal.x + 5, modal.y + 9, modal.width, modal.height},
                      {33, 34, 38, 38}, 18);
    surface.fill_rect(modal, panel, 18);
    surface.stroke_rect(modal, hairline, 1, 18);
    label(surface, "插件页面", {modal.x + 26, modal.y + 18, 180, 25}, 16, ink,
          680);
    const Rect close{modal.x + modal.width - 45, modal.y + 17, 30, 30};
    if (hovered(close)) surface.fill_rect(close, hover_fill, 8);
    draw_icon(surface, "window-close", close.x + 15, close.y + 15,
              hovered(close) ? danger : secondary);
    add_hit(close, WorkbenchActionKind::close_plugins);
    const std::array<std::string_view, 3> tabs = {"插件中心", "能力编排",
                                                  "我的插件"};
    float tab_x = modal.x + 26;
    for (std::size_t index = 0; index < tabs.size(); ++index) {
      const Rect tab{tab_x, modal.y + 54, 96, 42};
      if (index == 1) {
        label(surface, tabs[index], tab, 11, accent, 620, 1,
              white::TextAlign::center);
        surface.fill_rect({tab.x + 22, tab.y + tab.height - 2,
                           tab.width - 44, 2}, accent, 1);
      } else {
        label(surface, tabs[index], tab, 11, secondary, 500, 1,
              white::TextAlign::center);
      }
      add_hit(tab, WorkbenchActionKind::redraw);
      tab_x += 114;
    }
    surface.line(modal.x, modal.y + 96, modal.x + modal.width, modal.y + 96,
                 hairline);

    const float detail_width = 330;
    const Rect canvas{modal.x, modal.y + 97, modal.width - detail_width,
                      modal.height - 97};
    const Rect detail{canvas.x + canvas.width, canvas.y, detail_width,
                      canvas.height};
    surface.line(detail.x, detail.y, detail.x, detail.y + detail.height,
                 hairline);
    for (float gx = canvas.x + 22; gx < canvas.x + canvas.width; gx += 18)
      for (float gy = canvas.y + 68; gy < canvas.y + canvas.height; gy += 18)
        surface.fill_circle(gx, gy, 0.65F, {214, 216, 220, 170});

    const Rect graph_search{canvas.x + 24, canvas.y + 20, 270, 36};
    surface.fill_rect(graph_search, panel, 8);
    surface.stroke_rect(graph_search, hairline, 1, 8);
    draw_icon(surface, "search", graph_search.x + 17, graph_search.y + 18,
              muted);
    label(surface, "搜索插件或能力…",
          {graph_search.x + 38, graph_search.y + 8, 210, 20}, 10, muted, 430);
    button({canvas.x + canvas.width - 216, canvas.y + 20, 92, 36},
           "+ 添加节点", WorkbenchActionKind::redraw);
    button({canvas.x + canvas.width - 114, canvas.y + 20, 90, 36},
           "自动布局", WorkbenchActionKind::redraw);

    const Rect terminal{canvas.x + canvas.width * 0.40F, canvas.y + 248, 188,
                        88};
    const std::array<Rect, 6> nodes = {
        Rect{canvas.x + 74, canvas.y + 92, 188, 80},
        Rect{canvas.x + canvas.width - 262, canvas.y + 92, 188, 80},
        Rect{canvas.x + 28, canvas.y + 250, 188, 80},
        Rect{canvas.x + canvas.width - 216, canvas.y + 250, 188, 80},
        Rect{canvas.x + 74, canvas.y + 414, 188, 80},
        Rect{canvas.x + canvas.width - 262, canvas.y + 414, 188, 80}};
    const std::array<std::pair<std::string_view, std::string_view>, 6>
        node_text = {{{"文件系统", "读写文件与目录"},
                      {"代码分析", "解析代码结构与符号"},
                      {"工作区观察器", "监听文件变更"},
                      {"文档检索", "检索项目内文档"},
                      {"时间记忆", "跨会话记忆存储"},
                      {"测试运行", "发现并运行测试"}}};
    for (const auto &node : nodes) {
      const float start_x = node.x < terminal.x ? node.x + node.width : node.x;
      const float end_x = node.x < terminal.x ? terminal.x : terminal.x + terminal.width;
      surface.line(start_x, node.y + node.height / 2, end_x,
                   terminal.y + terminal.height / 2,
                   {173, 178, 188, 255}, 1.1F);
    }
    const auto draw_node = [&](Rect node, std::string_view title,
                               std::string_view description, bool selected) {
      surface.fill_rect(node, panel, 11);
      surface.stroke_rect(node, selected ? accent : hairline,
                          selected ? 1.5F : 1.0F, 11);
      draw_icon(surface, selected ? "plugin" : "file", node.x + 24,
                node.y + 30, selected ? accent : secondary);
      label(surface, title, {node.x + 45, node.y + 15, node.width - 55, 20},
            11, ink, 620);
      label(surface, description,
            {node.x + 45, node.y + 38, node.width - 55, 18}, 9, muted, 430);
      surface.fill_circle(node.x + 45, node.y + node.height - 14, 3, success);
      label(surface, "启用",
            {node.x + 54, node.y + node.height - 23, 50, 18}, 8, success,
            500);
      add_hit(node, WorkbenchActionKind::redraw);
    };
    for (std::size_t index = 0; index < nodes.size(); ++index)
      draw_node(nodes[index], node_text[index].first, node_text[index].second,
                false);
    draw_node(terminal, "终端执行", "执行命令与脚本", true);
    const Rect zoom{canvas.x + 24, canvas.y + canvas.height - 52, 245, 36};
    surface.fill_rect(zoom, panel, 8);
    surface.stroke_rect(zoom, hairline, 1, 8);
    label(surface, "↖     ✋      −     100%     +     ⛶", zoom, 10,
          secondary, 500, 1, white::TextAlign::center);

    const Rect plugin_mark{detail.x + 22, detail.y + 20, 44, 44};
    surface.fill_rect(plugin_mark, {60, 63, 69, 255}, 8);
    label(surface, ">_", plugin_mark, 13, {255, 255, 255, 255}, 650, 1,
          white::TextAlign::center);
    label(surface, "终端执行", {detail.x + 78, detail.y + 21, 150, 22}, 13,
          ink, 650);
    label(surface, "v1.2.0", {detail.x + 78, detail.y + 43, 100, 18}, 9,
          muted, 450);
    surface.fill_rect({detail.x + detail.width - 86, detail.y + 24, 64, 24},
                      {235, 248, 239, 255}, 12);
    label(surface, "● 启用中",
          {detail.x + detail.width - 84, detail.y + 27, 60, 18}, 8, success,
          550, 1, white::TextAlign::center);
    label(surface, "在隔离环境中执行命令、脚本并返回结果。",
          {detail.x + 22, detail.y + 78, detail.width - 44, 38}, 9, secondary,
          430, 2);
    label(surface, "权限", {detail.x + 22, detail.y + 126, 120, 20}, 10, ink,
          620);
    label(surface, "shell:execute    fs:read    env:read",
          {detail.x + 22, detail.y + 150, detail.width - 44, 25}, 8, secondary,
          500);
    const auto draw_schema = [&](float y, std::string_view title,
                                 std::string_view code) {
      label(surface, title, {detail.x + 22, y, 160, 18}, 9, ink, 600);
      const Rect card{detail.x + 22, y + 23, detail.width - 44, 126};
      surface.fill_rect(card, {248, 249, 250, 255}, 7);
      surface.stroke_rect(card, hairline, 1, 7);
      label(surface, code, {card.x + 10, card.y + 9, card.width - 20, 105}, 8,
            secondary, 430, 6);
    };
    draw_schema(detail.y + 184, "输入 Schema",
                "{\n  type: object,\n  properties: { command, cwd, env },\n  required: [command]\n}");
    draw_schema(detail.y + 354, "输出 Schema",
                "{\n  type: object,\n  properties: { code, stdout, stderr }\n}");
    const float action_y = detail.y + detail.height - 54;
    button({detail.x + 22, action_y, 75, 34}, "停用",
           WorkbenchActionKind::redraw, false, danger);
    button({detail.x + 108, action_y, 75, 34}, "配置",
           WorkbenchActionKind::open_config_file);
    button({detail.x + 194, action_y, detail.width - 216, 34}, "查看文档",
           WorkbenchActionKind::redraw);
  }

  // Settings is a centered, modal workspace. The view edits a draft owned by
  // App; Save atomically updates the JSON configuration set.
  settings_modal_bounds_ = {};
  settings_editor_bounds_ = {};
  settings_editor_text_.clear();
  settings_editor_field_.clear();
  if (settings_open_) {
    hits_.clear();
    surface.fill_rect({0, 0, width, height}, {242, 243, 244, 210});
    const float modal_width = std::min(1040.0F, width - 72.0F);
    const float modal_height = std::min(720.0F, height - 72.0F);
    const Rect modal{(width - modal_width) / 2, (height - modal_height) / 2,
                     modal_width, modal_height};
    settings_modal_bounds_ = modal;
    surface.fill_rect({modal.x + 5, modal.y + 8, modal.width, modal.height},
                      {33, 34, 38, 45}, 18);
    surface.fill_rect(modal, {255, 255, 254, 255}, 18);
    surface.stroke_rect(modal, {218, 218, 215, 255}, 1, 18);
    draw_icon(surface, "branch", modal.x + 28, modal.y + 30, ink);
    label(surface, "设置", {modal.x + 49, modal.y + 18, 100, 24}, 15, ink, 650);

    const Rect open_config{modal.x + modal.width - 170, modal.y + 15, 122, 30};
    if (hovered(open_config))
      surface.fill_rect(open_config, hover_fill, 9);
    surface.stroke_rect(open_config,
                        hovered(open_config) ? hover_border : hairline, 1, 9);
    label(surface, "打开配置文件", open_config, 10, ink, 500, 1,
          white::TextAlign::center);
    add_hit(open_config, WorkbenchActionKind::open_config_file);
    const Rect close{modal.x + modal.width - 40, modal.y + 14, 28, 30};
    if (hovered(close))
      surface.fill_rect(close, hover_fill, 8);
    draw_icon(surface, "window-close", close.x + 14, close.y + 15,
              hovered(close) ? danger : secondary);
    add_hit(close, WorkbenchActionKind::close_settings);

    const float navigation_width = 184;
    const Rect navigation{modal.x + 12, modal.y + 56, navigation_width,
                          modal.height - 70};
    surface.line(navigation.x + navigation.width, navigation.y,
                 navigation.x + navigation.width,
                 navigation.y + navigation.height, hairline);
    struct SettingsNav {
      std::string_view id;
      std::string_view icon;
      std::string_view label;
    };
    constexpr SettingsNav settings_nav[] = {
        {"general", "settings", "通用"},
        {"appearance", "model", "外观"},
        {"models", "pulse", "模型与代理"},
        {"workspace", "folder", "工作空间"},
        {"shortcuts", "file", "快捷键"},
        {"about", "branch", "关于"}};
    float nav_y = navigation.y + 12;
    for (const auto &item : settings_nav) {
      const Rect row{navigation.x + 5, nav_y, navigation.width - 18, 39};
      const auto selected = settings_tab_ == item.id;
      if (selected || hovered(row))
        surface.fill_rect(row, selected ? selected_fill : hover_fill, 9);
      draw_icon(surface, item.icon, row.x + 18, row.y + 20,
                selected ? ink : secondary);
      label(surface, item.label, {row.x + 38, row.y + 9, row.width - 45, 21},
            11, selected ? ink : secondary, selected ? 650 : 500);
      hits_.push_back(
          {row, WorkbenchActionKind::settings_tab, {}, std::string(item.id)});
      nav_y += 44;
    }

    const Rect content{navigation.x + navigation.width + 24, modal.y + 66,
                       modal.width - navigation.width - 60, modal.height - 136};
    const auto draw_section_title = [&](std::string_view title,
                                        std::string_view description) {
      label(surface, title, {content.x, content.y, content.width, 25}, 15, ink,
            650);
      label(surface, description,
            {content.x, content.y + 31, content.width, 35}, 10, muted, 400, 2);
    };
    const auto draw_toggle = [&](float y, std::string_view label_text,
                                 std::string_view description,
                                 std::string_view key, bool value) {
      const Rect row{content.x, y, content.width, 48};
      if (hovered(row))
        surface.fill_rect(row, {249, 249, 248, 255}, 8);
      label(surface, label_text, {row.x + 10, row.y + 7, row.width - 90, 20},
            11, ink, 550);
      label(surface, description, {row.x + 10, row.y + 26, row.width - 90, 17},
            9, muted, 400);
      const Rect toggle{row.x + row.width - 51, row.y + 12, 39, 23};
      surface.fill_rect(toggle, value ? accent : Color{205, 206, 208, 255}, 12);
      surface.fill_circle(toggle.x + (value ? 28 : 11), toggle.y + 11.5F, 8.5F,
                          {255, 255, 255, 255});
      hits_.push_back({row,
                       WorkbenchActionKind::set_setting,
                       {},
                       std::string(key) + "=" + (value ? "false" : "true")});
    };
    const auto draw_choice = [&](float y, std::string_view label_text,
                                 std::string_view value,
                                 std::string_view encoded_next) {
      const Rect row{content.x, y, content.width, 43};
      if (hovered(row))
        surface.fill_rect(row, {249, 249, 248, 255}, 8);
      label(surface, label_text, {row.x + 10, row.y + 10, 190, 20}, 11, ink,
            550);
      label(surface, value, {row.x + row.width - 185, row.y + 10, 155, 20}, 10,
            secondary, 500, 1, white::TextAlign::right);
      draw_icon(surface, "chevron", row.x + row.width - 16, row.y + 21, muted);
      hits_.push_back({row,
                       WorkbenchActionKind::set_setting,
                       {},
                       std::string(encoded_next)});
    };
    const auto draw_field = [&](float y, std::string_view field,
                                std::string_view label_text,
                                std::string_view hint = {}) {
      label(surface, label_text, {content.x, y, content.width, 18}, 9,
            secondary, 550);
      const Rect input{content.x, y + 21, content.width, 34};
      const auto active = frame.active_settings_field == field;
      surface.fill_rect(input, {255, 255, 255, 255}, 7);
      surface.stroke_rect(
          input, active ? accent : (hovered(input) ? hover_border : hairline),
          active ? 1.3F : 1.0F, 7);
      const auto value = setting_value(frame.settings, field);
      const Rect editor{input.x + 11, input.y + 8, input.width - 22, 19};
      if (value.empty() && !hint.empty())
        label(surface, hint, editor, 10, muted, 400);
      else if (active) {
        draw_editor_text(surface, value, editor, frame.editor_cursor,
                         frame.selection_start, frame.selection_end,
                         frame.settings_field_focused, frame.caret_visible, 10,
                         1);
        settings_editor_bounds_ = editor;
        settings_editor_text_ = value;
        settings_editor_field_ = std::string(field);
      } else {
        label(surface, value, editor, 10, ink, 450);
      }
      hits_.push_back({input,
                       WorkbenchActionKind::focus_settings_field,
                       {},
                       std::string(field)});
    };

    if (settings_tab_ == "general") {
      draw_section_title("通用", "配置启动、语言和运行时更新行为。");
      float y = content.y + 76;
      draw_choice(y, "界面语言", frame.settings.language,
                  "language=" + std::string(frame.settings.language == "zh-CN"
                                                ? "en-US"
                                                : "zh-CN"));
      y += 54;
      draw_toggle(y, "启动时恢复工作区", "打开上次使用的工作区和会话",
                  "auto_scroll", frame.settings.auto_scroll);
      y += 56;
      draw_toggle(y, "自动检查更新", "定期检查 Tokmon 与 Snow 组件更新",
                  "restart_enabled", frame.settings.restart_enabled);
      y += 62;
      draw_field(y, "request_timeout_ms", "请求超时（毫秒）", "300000");
    } else if (settings_tab_ == "appearance") {
      draw_section_title("外观", "统一调整主题、字号、界面密度和代码字体。");
      float y = content.y + 76;
      const auto next_theme = frame.settings.theme == "system"  ? "light"
                              : frame.settings.theme == "light" ? "dark"
                                                                : "system";
      draw_choice(y, "主题模式", frame.settings.theme,
                  "theme=" + std::string(next_theme));
      y += 62;
      const auto draw_segment = [&](std::string_view title,
                                    std::array<std::string_view, 3> choices,
                                    std::size_t selected) {
        label(surface, title, {content.x + 10, y, 180, 20}, 11, ink, 550);
        const Rect rail{content.x + content.width - 310, y - 5, 300, 32};
        surface.fill_rect(rail, selected_fill, 8);
        const float segment_width = rail.width / 3;
        for (std::size_t index = 0; index < choices.size(); ++index) {
          const Rect segment{rail.x + segment_width * static_cast<float>(index),
                             rail.y, segment_width, rail.height};
          if (index == selected) {
            surface.fill_rect({segment.x + 2, segment.y + 2,
                               segment.width - 4, segment.height - 4},
                              panel, 7);
            surface.stroke_rect({segment.x + 2, segment.y + 2,
                                 segment.width - 4, segment.height - 4},
                                hairline, 1, 7);
          }
          label(surface, choices[index], segment, 10,
                index == selected ? ink : secondary,
                index == selected ? 600 : 450, 1,
                white::TextAlign::center);
        }
        y += 50;
      };
      draw_segment("字体大小", {"小", "中（推荐）", "大"}, 1);
      draw_segment("界面密度", {"紧凑", "舒适", "宽松"}, 1);
      draw_choice(y, "代码字体", "JetBrains Mono", "redraw");
    } else if (settings_tab_ == "models") {
      draw_section_title("模型与代理",
                         "配置默认模型、提供方和代理执行参数；密钥不会写入 JSON。");
      float y = content.y + 71;
      const Rect provider_card{content.x, y, content.width, 48};
      surface.fill_rect(provider_card, {250, 250, 249, 255}, 9);
      surface.stroke_rect(provider_card, hairline, 1, 9);
      surface.fill_circle(provider_card.x + 18, provider_card.y + 24, 4,
                          success);
      label(surface, frame.settings.provider_name,
            {provider_card.x + 32, provider_card.y + 8,
             provider_card.width - 44, 20},
            11, ink, 650);
      label(surface, frame.settings.provider_kind,
            {provider_card.x + 32, provider_card.y + 26,
             provider_card.width - 44, 16},
            9, muted, 450);
      y += 62;
      draw_field(y, "provider_id", "Provider ID", "default");
      y += 66;
      draw_field(y, "provider_name", "显示名称", "Default provider");
      y += 66;
      draw_field(y, "endpoint", "API 地址", "https://…/v1/chat/completions");
      y += 66;
      draw_field(y, "api_key_env", "API 密钥环境变量", "OPENAI_API_KEY");
      y += 66;
      draw_field(y, "model", "默认模型", "gpt-5");
      y += 64;
      const Rect add_provider{content.x, y, content.width, 32};
      if (hovered(add_provider))
        surface.fill_rect(add_provider, {247, 248, 250, 255}, 8);
      surface.stroke_rect(
          add_provider, hovered(add_provider) ? hover_border : hairline, 1, 8);
      label(surface, "+ 新增自定义提供方", add_provider, 10, secondary, 550, 1,
            white::TextAlign::center);
      hits_.push_back({add_provider,
                       WorkbenchActionKind::set_setting,
                       {},
                       "provider_template=custom"});
    } else if (settings_tab_ == "plugins") {
      draw_section_title("插件",
                         "Arche 在运行时组合能力；核心微内核插件保持锁定。");
      float y = content.y + 72;
      for (std::size_t index = 0; index < frame.settings.plugins.size();
           ++index) {
        const auto &plugin = frame.settings.plugins[index];
        if (y + 54 > content.y + content.height)
          break;
        const Rect row{content.x, y, content.width, 49};
        if (hovered(row))
          surface.fill_rect(row, {249, 249, 248, 255}, 8);
        surface.fill_circle(row.x + 14, row.y + 17, 3.5F,
                            plugin.disabled ? muted : success);
        label(surface, plugin.instance, {row.x + 27, row.y + 6, 145, 20}, 11,
              ink, 650);
        label(surface, plugin.realm + " · " + plugin.package,
              {row.x + 27, row.y + 25, row.width - 105, 17}, 9, muted, 400);
        if (plugin.required) {
          label(surface, "核心 · 已锁定",
                {row.x + row.width - 94, row.y + 14, 80, 18}, 9, muted, 550, 1,
                white::TextAlign::right);
        } else {
          const Rect toggle{row.x + row.width - 52, row.y + 13, 39, 23};
          const auto enabled = !plugin.disabled;
          surface.fill_rect(toggle,
                            enabled ? accent : Color{205, 206, 208, 255}, 12);
          surface.fill_circle(toggle.x + (enabled ? 28 : 11), toggle.y + 11.5F,
                              8.5F, {255, 255, 255, 255});
          hits_.push_back(
              {row, WorkbenchActionKind::set_setting, {}, "plugin", index});
        }
        y += 54;
      }
    } else if (settings_tab_ == "workspace") {
      draw_section_title("工作空间",
                         "控制工作区会话行为、轨迹记录与默认执行方式。");
      float y = content.y + 76;
      struct Preset {
        std::string_view id;
        std::string_view label;
        std::string_view description;
      };
      constexpr Preset presets[] = {
          {"balanced", "均衡", "规划、执行与审阅保持平衡"},
          {"autonomous", "自主", "允许更长的自进化执行轨迹"},
          {"review", "审阅", "优先分析和确认，减少主动变更"}};
      const float card_width = (content.width - 16) / 3;
      float x = content.x;
      for (const auto &preset : presets) {
        const Rect card{x, y, card_width, 82};
        const auto selected = frame.settings.agent_preset == preset.id;
        surface.fill_rect(
            card,
            selected ? Color{241, 245, 253, 255}
                     : (hovered(card) ? Color{249, 249, 248, 255} : panel),
            10);
        surface.stroke_rect(card, selected ? accent : hairline,
                            selected ? 1.3F : 1.0F, 10);
        label(surface, preset.label,
              {card.x + 10, card.y + 10, card.width - 20, 20}, 11, ink, 650);
        label(surface, preset.description,
              {card.x + 10, card.y + 33, card.width - 20, 36}, 9, muted, 400,
              2);
        hits_.push_back({card,
                         WorkbenchActionKind::set_setting,
                         {},
                         "agent_preset=" + std::string(preset.id)});
        x += card_width + 8;
      }
      y += 102;
      draw_field(y, "max_steps", "每轮最大步骤", "32");
      y += 72;
      const auto next_permission =
          frame.settings.default_permission == "ask"     ? "allow"
          : frame.settings.default_permission == "allow" ? "deny"
                                                         : "ask";
      draw_choice(y, "可变更操作默认权限", frame.settings.default_permission,
                  "default_permission=" + std::string(next_permission));
      y += 54;
      draw_toggle(y, "保留完整原始轨迹", "为归档和审计保留 Snow raw vault 数据",
                  "raw_trace", frame.settings.raw_trace);
    } else if (settings_tab_ == "shortcuts") {
      draw_section_title("快捷键", "查看工作台中常用操作的键盘映射。");
      struct ShortcutRow {
        std::string_view label;
        std::string_view keys;
      };
      constexpr ShortcutRow shortcuts[] = {
          {"搜索项目或会话", "Ctrl  K"},
          {"创建新会话", "Ctrl  N"},
          {"打开设置", "Ctrl  ,"},
          {"切换轨迹视图", "Ctrl  Shift  T"},
          {"发送消息", "Enter"},
          {"消息内换行", "Shift  Enter"}};
      float y = content.y + 72;
      for (const auto &shortcut : shortcuts) {
        const Rect row{content.x, y, content.width, 48};
        if (hovered(row)) surface.fill_rect(row, hover_fill, 8);
        label(surface, shortcut.label, {row.x + 10, row.y + 13, 250, 20},
              11, ink, 500);
        const Rect key{row.x + row.width - 180, row.y + 10, 168, 28};
        surface.fill_rect(key, selected_fill, 7);
        surface.stroke_rect(key, hairline, 1, 7);
        label(surface, shortcut.keys, key, 9, secondary, 550, 1,
              white::TextAlign::center);
        y += 52;
      }
    } else {
      draw_section_title("关于", "Tokmon · 基于 Arche 微内核与 White 原生 UI 引擎。");
      const Rect brand_card{content.x, content.y + 78, content.width, 118};
      surface.fill_rect(brand_card, {250, 250, 249, 255}, 12);
      surface.stroke_rect(brand_card, hairline, 1, 12);
      surface.fill_circle(brand_card.x + 42, brand_card.y + 42, 25,
                          selected_fill);
      draw_icon(surface, "branch", brand_card.x + 42, brand_card.y + 42, ink);
      label(surface, "Tokmon", {brand_card.x + 80, brand_card.y + 24, 220, 24},
            17, ink, 700);
      label(surface, "Arche Agent OS 原生工作台",
            {brand_card.x + 80, brand_card.y + 52, 280, 20}, 10, secondary, 450);
      label(surface, "White · Yoga · Skia · SDL3",
            {brand_card.x + 80, brand_card.y + 77, 300, 18}, 9, muted, 450);
      const float info_y = brand_card.y + 144;
      label(surface, "版本", {content.x + 10, info_y, 110, 20}, 10, muted, 450);
      label(surface, "development", {content.x + 150, info_y, 240, 20}, 10,
            ink, 550);
      label(surface, "渲染后端", {content.x + 10, info_y + 42, 110, 20}, 10,
            muted, 450);
      label(surface, "Skia GPU / CPU", {content.x + 150, info_y + 42, 240, 20},
            10, ink, 550);
      label(surface, "架构", {content.x + 10, info_y + 84, 110, 20}, 10, muted,
            450);
      label(surface, "插件化微内核", {content.x + 150, info_y + 84, 240, 20},
            10, ink, 550);
    }

    const Rect footer{modal.x + navigation_width + 36,
                      modal.y + modal.height - 60,
                      modal.width - navigation_width - 54, 44};
    surface.line(footer.x, footer.y - 5, footer.x + footer.width, footer.y - 5,
                 hairline);
    const Rect cancel{footer.x + footer.width - 178, footer.y + 4, 76, 32};
    const Rect save{footer.x + footer.width - 92, footer.y + 4, 82, 32};
    button(cancel, "取消", WorkbenchActionKind::close_settings);
    button(save, "保存设置", WorkbenchActionKind::save_settings, true);
  }
  active_hover_region_ = hover_region_at(pointer_x_, pointer_y_);
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
        return {WorkbenchActionKind::focus_settings_field,
                settings_editor_field_, 0,
                editor_offset_at(event.x, event.y, settings_editor_bounds_,
                                 settings_editor_text_),
                false};
      }
      selecting_input_ = false;
      selecting_editor_.clear();
      return {};
    }
    if (archive_open_) {
      if (filter_editor_bounds_.contains(event.x, event.y)) {
        request_redraw(filter_editor_bounds_);
        selecting_input_ = true;
        selecting_filter_ = true;
        selecting_editor_ = "filter";
        return {WorkbenchActionKind::focus_filter,
                {},
                0,
                editor_offset_at(event.x, event.y, filter_editor_bounds_,
                                 filter_editor_text_),
                false};
      }
      selecting_input_ = false;
      selecting_editor_.clear();
      return {};
    }
    if (plugins_open_) return {};
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
      return {WorkbenchActionKind::focus_message,
              {},
              0,
              editor_offset_at(event.x, event.y, message_editor_bounds_,
                               message_editor_text_),
              false};
    }
    if (filter_editor_bounds_.contains(event.x, event.y)) {
      request_redraw(filter_editor_bounds_);
      selecting_input_ = true;
      selecting_filter_ = true;
      selecting_editor_ = "filter";
      return {WorkbenchActionKind::focus_filter,
              {},
              0,
              editor_offset_at(event.x, event.y, filter_editor_bounds_,
                               filter_editor_text_),
              false};
    }
    if (trajectory_search_bounds_.contains(event.x, event.y)) {
      request_redraw(trajectory_search_bounds_);
      selecting_input_ = true;
      selecting_editor_ = "trajectory";
      return {WorkbenchActionKind::focus_trajectory_search,
              {},
              0,
              editor_offset_at(event.x, event.y, trajectory_search_bounds_,
                               trajectory_search_text_),
              false};
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
      const auto content_reserve =
          last_layout_.menu_bar.width >= viewer_visible_breakpoint &&
                  !viewer_collapsed_
              ? 940.0F
              : 640.0F;
      const auto max_sidebar =
          std::max(176.0F, last_layout_.menu_bar.width - content_reserve);
      const auto next_width = std::clamp(event.x, 176.0F, max_sidebar);
      const bool changed = next_width != sidebar_width_ || sidebar_collapsed_ ||
                           !sidebar_manually_sized_;
      sidebar_width_ = next_width;
      sidebar_collapsed_ = false;
      sidebar_manually_sized_ = true;
      if (changed) request_redraw();
      return {changed ? WorkbenchActionKind::redraw
                      : WorkbenchActionKind::none,
              {}, 0, 0, false, true};
    }
    if (resizing_viewer_) {
      const auto available =
          last_layout_.menu_bar.width - last_layout_.sidebar.width;
      const auto max_viewer = std::max(320.0F, available - 500.0F);
      const auto next_width = std::clamp(
          last_layout_.menu_bar.width - event.x, 320.0F, max_viewer);
      const bool changed = next_width != viewer_width_ || viewer_collapsed_ ||
                           !viewer_manually_sized_;
      viewer_width_ = next_width;
      viewer_collapsed_ = false;
      viewer_manually_sized_ = true;
      if (changed) request_redraw();
      return {changed ? WorkbenchActionKind::redraw
                      : WorkbenchActionKind::none,
              {}, 0, 0, false, true};
    }
    if (selecting_input_) {
      const auto &bounds =
          selecting_editor_ == "settings"     ? settings_editor_bounds_
          : selecting_editor_ == "trajectory" ? trajectory_search_bounds_
          : selecting_editor_ == "filter"     ? filter_editor_bounds_
                                              : message_editor_bounds_;
      const auto &text = selecting_editor_ == "settings" ? settings_editor_text_
                         : selecting_editor_ == "trajectory"
                             ? trajectory_search_text_
                         : selecting_editor_ == "filter" ? filter_editor_text_
                                                         : message_editor_text_;
      const auto cursor = editor_offset_at(event.x, event.y, bounds, text);
      if (cursor == editor_cursor_) return {};
      editor_cursor_ = cursor;
      request_redraw(bounds);
      return {WorkbenchActionKind::set_editor_cursor,           {},   0,
              cursor, true, false};
    }
    const bool over_splitter =
        last_layout_.sidebar_splitter.contains(event.x, event.y) ||
        last_layout_.viewer_splitter.contains(event.x, event.y);
    WorkbenchAction result;
    result.kind = hover_changed ? WorkbenchActionKind::redraw
                                : WorkbenchActionKind::none;
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
    result.kind = hover_changed ? WorkbenchActionKind::redraw
                                : WorkbenchActionKind::none;
    if (pointer_cursor_active_) {
      pointer_cursor_active_ = false;
      result.pointer_cursor = false;
    }
    return result;
  }
  if (event.type == "wheel") {
    if (settings_open_ || archive_open_ || plugins_open_) return {};
    if (last_layout_.sidebar.contains(event.x, event.y) && event.y >= 301) {
      const auto next = std::clamp(session_scroll_ + event.delta_y, 0.0F,
                                   session_max_scroll_);
      if (next == session_scroll_) return {};
      session_scroll_ = next;
      request_redraw(last_layout_.sidebar);
      return {WorkbenchActionKind::redraw};
    }
    if (trajectory_open_ &&
        last_layout_.conversation.contains(event.x, event.y) &&
        event.y >= last_layout_.conversation.y + 63) {
      const auto next = std::clamp(trajectory_scroll_ + event.delta_y, 0.0F,
                                   trajectory_max_scroll_);
      if (next == trajectory_scroll_) return {};
      trajectory_scroll_ = next;
      request_redraw(last_layout_.conversation);
      return {WorkbenchActionKind::redraw};
    }
    if (last_layout_.timeline.contains(event.x, event.y)) {
      const auto next = std::clamp(timeline_scroll_ + event.delta_y, 0.0F,
                                   timeline_max_scroll_);
      if (next == timeline_scroll_) return {};
      timeline_scroll_ = next;
      follow_tail_ = timeline_scroll_ >= timeline_max_scroll_ - 2;
      request_redraw(last_layout_.timeline);
      return {WorkbenchActionKind::redraw};
    }
    if (last_layout_.document.contains(event.x, event.y)) {
      const auto next = std::clamp(document_scroll_ + event.delta_y, 0.0F,
                                   document_max_scroll_);
      if (next == document_scroll_) return {};
      document_scroll_ = next;
      request_redraw(last_layout_.document);
      return {WorkbenchActionKind::redraw};
    }
    return {};
  }
  if (event.type != "click")
    return {};
  // Clicks can open overlays or change product state through many commands;
  // conservatively repaint once. Pointer motion and scrolling stay regional.
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
  const Rect menu_headers{dp(160), dp(13), dp(228), dp(38)};
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
    if (target.action == WorkbenchActionKind::open_config_file) {
      profile_menu_open_ = false;
      settings_open_ = false;
      archive_open_ = false;
      plugins_open_ = false;
      return {WorkbenchActionKind::open_config_file};
    }
    if (target.action == WorkbenchActionKind::settings_tab) {
      settings_tab_ = target.value;
      return {WorkbenchActionKind::settings_tab, target.value};
    }
    if (target.action == WorkbenchActionKind::show_conversation) {
      trajectory_open_ = false;
      trajectory_search_bounds_ = {};
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
    if (target.action == WorkbenchActionKind::set_trajectory_filter) {
      trajectory_filter_ = target.value;
      trajectory_scroll_ = 0;
      return {WorkbenchActionKind::redraw};
    }
    if (target.action == WorkbenchActionKind::toggle_trajectory_event) {
      const auto seq = static_cast<std::uint64_t>(target.index);
      if (expanded_trajectory_events_.contains(seq))
        expanded_trajectory_events_.erase(seq);
      else
        expanded_trajectory_events_.insert(seq);
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
