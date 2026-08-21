#include <tokmon/workbench.hpp>
#include <tokmon/workbench_document.hpp>

#include <tokmon/common/files.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <ranges>
#include <sstream>

namespace tokmon::desktop {
namespace {

using white::Color;
using white::RasterSurface;
using white::Rect;

// Tokmon UI design tokens (docs/Tokmon UI prototype) - warm stone & gold.
constexpr Color app_background{245, 245, 244, 255};       // #f5f5f4
constexpr Color sidebar_background{249, 249, 248, 255};   // #f9f9f8
constexpr Color main_background{250, 250, 249, 255};      // #fafaf9
constexpr Color panel{255, 255, 255, 255};                // #ffffff
constexpr Color ink{28, 25, 23, 255};                     // #1c1917
constexpr Color ink_soft{41, 37, 36, 255};                // #292524
constexpr Color secondary{68, 64, 60, 255};               // #44403c
constexpr Color tertiary{87, 83, 78, 255};                // #57534e
constexpr Color muted{120, 113, 108, 255};                // #78716c
constexpr Color faint{168, 162, 158, 255};                // #a8a29e
constexpr Color hairline{231, 229, 228, 255};             // #e7e5e4
constexpr Color hairline_soft{245, 245, 244, 255};        // #f5f5f4
constexpr Color hover_fill{240, 238, 230, 255};           // #f0eee6
constexpr Color hover_quiet{245, 245, 244, 255};          // #f5f5f4
constexpr Color selected_fill{254, 243, 214, 255};        // #fef3d6
constexpr Color selected_border{253, 230, 138, 255};      // #fde68a
constexpr Color gold_accent{245, 166, 35, 255};           // #f5a623
constexpr Color gold_focus{245, 158, 11, 255};            // #f59e0b (Figma focus/active borders)
constexpr Color amber{217, 119, 6, 255};                  // #d97706
constexpr Color amber_deep{180, 83, 9, 255};              // #b45309
constexpr Color gold_dark{133, 87, 2, 255};               // #855702
constexpr Color gold_pill_bg{254, 245, 216, 255};         // #fef5d8
constexpr Color gold_pill_border{245, 228, 171, 255};     // #f5e4ab
constexpr Color gold_pill_hover{253, 238, 169, 255};      // #fdeea9
constexpr Color success{34, 197, 94, 255};                // #22c55e
constexpr Color success_deep{22, 163, 74, 255};           // #16a34a
constexpr Color success_text{21, 128, 61, 255};           // #15803d
constexpr Color success_bg{240, 253, 244, 255};           // #f0fdf4
constexpr Color success_border{187, 247, 208, 255};       // #bbf7d0
constexpr Color danger{220, 38, 38, 255};                 // #dc2626 (Figma red-600)
constexpr Color info_blue{2, 132, 199, 255};              // #0284c7
constexpr Color keyword_purple{130, 0, 219, 255};         // #8200db (Figma fill_eaef5054)
constexpr Color string_emerald{0, 122, 85, 255};          // #007a55 (Figma fill_ccde559c)
constexpr Color docstring_orange{187, 77, 0, 255};        // #bb4d00 (Figma docstring)
constexpr Color comment_gray{153, 161, 175, 255};         // #99a1af (Figma comment)

constexpr float design_density = 0.8F;
constexpr float dp(float value) { return value * design_density; }

float label(RasterSurface &surface, std::string_view value, const Rect &rect,
            float size = 13, Color color = ink, int weight = 400,
            std::size_t lines = 1,
            white::TextAlign align = white::TextAlign::left,
            bool monospace = false, float line_height = 1.28F) {
  return surface.paragraph(value, rect, dp(size), color, weight, line_height,
                           lines, align, monospace);
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
               Color{250, 250, 249, 255}, color);
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
                 {}, amber);
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

std::string_view language_label(std::string_view value) {
  if (value == "zh-CN" || value == "简体中文") return "简体中文";
  if (value == "en" || value == "en-US" || value == "English")
    return "English";
  return value;
}

std::string_view startup_label(std::string_view value) {
  if (value == "home" || value == "首页") return "首页";
  if (value == "last_session" || value == "上次打开的会话")
    return "上次打开的会话";
  return value;
}

std::string_view update_channel_label(std::string_view value) {
  if (value == "stable" || value == "稳定版") return "稳定版";
  if (value == "beta" || value == "测试版") return "测试版";
  return value;
}

std::string_view provider_mode_label(std::string_view value) {
  if (value == "official" || value == "Tokmon 官方") return "Tokmon 官方";
  if (value == "custom" || value == "自定义") return "自定义";
  return value;
}

std::string_view file_access_label(std::string_view value) {
  if (value == "trusted" || value == "受信路径") return "受信路径";
  if (value == "all" || value == "完全访问") return "完全访问";
  if (value == "ask" || value == "按需询问") return "按需询问";
  return value;
}

std::string_view approval_label(std::string_view value) {
  if (value == "auto" || value == "自动执行") return "自动执行";
  if (value == "on_demand" || value == "按需确认") return "按需确认";
  if (value == "deny" || value == "禁止执行") return "禁止执行";
  return value;
}

std::string_view index_mode_label(std::string_view value) {
  if (value == "standard" || value == "标准") return "标准";
  if (value == "deep" || value == "深度索引") return "深度索引";
  return value;
}

std::string_view theme_label(std::string_view value) {
  if (value == "light" || value == "浅色") return "浅色";
  if (value == "dark" || value == "深色") return "深色";
  return value;
}

std::string_view density_label(std::string_view value) {
  if (value == "compact" || value == "紧凑") return "紧凑";
  if (value == "comfortable" || value == "舒适") return "舒适";
  if (value == "loose" || value == "宽松") return "宽松";
  return value;
}

struct TrajectoryVisual {
  std::string_view label;
  Color color;
};

TrajectoryVisual trajectory_badge(std::string_view type) {
  if (type.starts_with("user/"))
    return {"USER", {2, 132, 199, 255}};
  if (type.starts_with("context/"))
    return {"CONTEXT", success_deep};
  if (type.starts_with("assistant/"))
    return {"ASSISTANT", {147, 51, 234, 255}};
  if (type.starts_with("tool/"))
    return {"TOOL", amber_deep};
  if (type.starts_with("model/") || type.starts_with("request/"))
    return {"MODEL", {124, 58, 237, 255}};
  if (type.starts_with("approval/"))
    return {"APPROVAL", amber_deep};
  if (type.find("error") != std::string_view::npos ||
      type.find("cancelled") != std::string_view::npos)
    return {"ERROR", {220, 38, 38, 255}};
  return {"SYSTEM", tertiary};
}

Color badge_background(std::string_view type) {
  if (type.starts_with("user/"))
    return {224, 242, 254, 255};
  if (type.starts_with("context/"))
    return {220, 252, 231, 255};
  if (type.starts_with("assistant/"))
    return {243, 232, 255, 255};
  if (type.starts_with("tool/") || type.starts_with("approval/"))
    return {254, 243, 214, 255};
  if (type.starts_with("model/") || type.starts_with("request/"))
    return {237, 233, 254, 255};
  if (type.find("error") != std::string_view::npos ||
      type.find("cancelled") != std::string_view::npos)
    return {254, 226, 226, 255};
  return {245, 245, 244, 255};
}

std::string trajectory_summary(const snow::TrajectoryEvent &event) {
  static constexpr std::string_view preferred[] = {
      "content", "message", "name", "reason", "status", "model"};
  for (const auto key : preferred) {
    if (!event.data.contains(key))
      continue;
    const auto &value = event.data.at(key);
    if (value.is_string())
      return utf8_prefix(value.get<std::string>(), 60);
    return utf8_prefix(value.dump(), 60);
  }
  if (event.data.empty())
    return "事件已提交到 Snow 持久轨迹";
  return utf8_prefix(event.data.dump(), 60);
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

bool visible_conversation_item(const ConversationItem &item) {
  return !(item.kind == ItemKind::status && item.title.starts_with("Event /"));
}

// Static workflow demo steps mirroring the prototype's execution log.
struct WorkflowStep {
  std::string_view time;
  std::string_view action;
  std::string_view argument;
  std::string_view icon;
  bool done;
  std::string_view output;
  int progress;
};

constexpr WorkflowStep workflow_steps[] = {
    {"10:21", "开始任务: 使用 ", "faster-whisper 转录音频并生成带时间戳字幕",
     "", true, "", -1},
    {"10:21", "探索文件夹 ", "C:\\Projects\\subtitle\\", "folder", true, "", -1},
    {"10:21", "读取文件 ", "config.yaml", "file", true, "", -1},
    {"10:22", "运行命令 ", "python -V", "terminal", true, "Python 3.10.11", -1},
    {"10:22", "运行命令 ", "pip show faster-whisper", "terminal", true,
     "faster-whisper 1.1.1", -1},
    {"10:23", "运行脚本 ", "transcribe.py --model large-v3-turbo",
     "file-code", true, "", 42},
    {"10:24", "生成文件 ", "output.srt", "file", false, "", -1},
};

float workflow_body_height() {
  float height = dp(8);
  for (const auto &step : workflow_steps) {
    height += dp(24);
    if (!step.output.empty())
      height += dp(24);
    if (step.progress >= 0)
      height += dp(76);
  }
  return height + dp(4);
}

bool is_text_file(const std::filesystem::path &path) {
  const auto extension = path.extension().string();
  static constexpr std::string_view extensions[] = {
      ".md", ".txt",   ".json", ".cpp", ".cc", ".c",  ".hpp",
      ".h",  ".cmake", ".mjs",  ".js",  ".ts", ".py", ".css", ".yaml", ".yml",
      ".srt"};
  return path.filename() == "CMakeLists.txt" ||
         std::ranges::find(extensions, extension) != std::end(extensions);
}

// Demo file contents shown in the code inspector (prototype data).
constexpr std::string_view demo_transcribe_py[] = {
    "import os",
    "import json",
    "from pathlib import Path",
    "from faster_whisper import WhisperModel",
    "",
    "def transcribe_audio(model_path: str, audio_path: str,",
    "                     output_srt: str, language: str = \"zh\",",
    "                     beam_size: int = 5, vad_filter: bool = True) -> dict:",
    "    \"\"\"使用 faster-whisper 进行音频转录（分段模式）并输出 SRT。\"\"\"",
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
    "    return {\"segments\": len(results), \"language\": info.language}",
};

constexpr std::string_view demo_config_yaml[] = {
    "# Faster-Whisper Subtitle Config",
    "model:",
    "  name: \"faster-whisper-large-v3-turbo\"",
    "  path: \"C:\\Models\\faster-whisper-large-v3-turbo\"",
    "  device: \"auto\"",
    "  compute_type: \"int8\"",
    "",
    "transcribe:",
    "  language: \"zh\"",
    "  beam_size: 5",
    "  vad_filter: true",
    "  min_silence_duration_ms: 400",
};

constexpr std::string_view demo_output_srt[] = {
    "1",
    "00:00:00,000 --> 00:00:02,340",
    "欢迎使用 faster-whisper 音频转录模型。",
    "",
    "2",
    "00:00:02,500 --> 00:00:05,800",
    "本视频将向您展示带时间轴字幕的自动生成过程。",
    "",
    "3",
    "00:00:06,100 --> 00:00:09,200",
    "任务已完成，共生成 96 条高精度时间轴字幕。",
};

// Sample SRT subtitles for the preview mode (prototype data).
struct SubtitleItem {
  int id;
  std::string_view start;
  std::string_view end;
  std::string_view text;
};

constexpr SubtitleItem subtitle_items[] = {
    {1, "00:00:00.000", "00:00:02.340", "欢迎使用 faster-whisper 音频转录模型。"},
    {2, "00:00:02.500", "00:00:05.800", "本视频将向您展示带时间轴字幕的自动生成过程。"},
    {3, "00:00:06.100", "00:00:09.200", "任务已完成，共生成 96 条高精度时间轴字幕。"},
    {4, "00:00:09.500", "00:00:12.800", "分段模式可自动检测说话停顿并精准对齐秒数。"},
    {5, "00:00:13.100", "00:00:16.400", "您可以随时导出 UTF-8 编码的 .srt 字幕文件。"},
};

std::string subtitle_export_text() {
  std::string result;
  for (const auto &item : subtitle_items) {
    result += std::to_string(item.id);
    result += "\n";
    result += item.start;
    result += " --> ";
    result += item.end;
    result += "\n";
    result += item.text;
    result += "\n\n";
  }
  return result;
}

bool is_keyword(std::string_view word) {
  static constexpr std::string_view keywords[] = {
      "import", "from",  "def",     "return", "for",   "in",   "as",   "and",
      "or",     "not",   "True",    "False",  "None",  "bool", "str",  "int",
      "dict",   "list",  "if",      "elif",   "else",  "with", "try",  "except",
      "class",  "while", "lambda",  "model",  "transcribe"};
  return std::ranges::find(keywords, word) != std::end(keywords);
}

// Lightweight Python/YAML syntax highlighter producing rich-text spans.
std::vector<white::RichTextSpan> code_spans(std::string_view line,
                                            float size) {
  std::vector<white::RichTextSpan> spans;
  const auto push = [&](std::string_view text, Color color, int weight) {
    if (text.empty())
      return;
    spans.push_back({std::string(text), dp(size), color, weight, true, {}});
  };
  const auto first_char = line.find_first_not_of(' ');
  const auto trimmed =
      first_char == std::string_view::npos ? std::string_view{}
                                           : line.substr(first_char);
  if (trimmed.starts_with('#') || trimmed.starts_with("//")) {
    push(line, comment_gray, 400);
    return spans;
  }
  if (line.find("\"\"\"") != std::string_view::npos) {
    push(line, docstring_orange, 500);
    return spans;
  }
  std::size_t cursor = 0;
  while (cursor < line.size()) {
    if (line[cursor] == '"' || line[cursor] == '\'') {
      const auto quote = line[cursor];
      auto end = cursor + 1;
      while (end < line.size() && line[end] != quote)
        ++end;
      if (end < line.size())
        ++end;
      push(line.substr(cursor, end - cursor), string_emerald, 400);
      cursor = end;
      continue;
    }
    if (line[cursor] == '#') {
      push(line.substr(cursor), comment_gray, 400);
      break;
    }
    if (std::isalpha(static_cast<unsigned char>(line[cursor])) ||
        line[cursor] == '_') {
      auto end = cursor;
      while (end < line.size() &&
             (std::isalnum(static_cast<unsigned char>(line[end])) ||
              line[end] == '_'))
        ++end;
      const auto word = line.substr(cursor, end - cursor);
      if (is_keyword(word))
        push(word, keyword_purple, 650);
      else
        push(word, ink, 400);
      cursor = end;
      continue;
    }
    auto end = cursor + 1;
    while (end < line.size() && line[end] != '"' && line[end] != '\'' &&
           line[end] != '#' &&
           !std::isalpha(static_cast<unsigned char>(line[end])) &&
           line[end] != '_')
      ++end;
    push(line.substr(cursor, end - cursor), ink, 400);
    cursor = end;
  }
  if (spans.empty())
    push(line, ink, 400);
  return spans;
}

void draw_icon(RasterSurface &surface, std::string_view name, float x, float y,
               Color color) {
  if (name == "plus") {
    surface.line(x - 5, y, x + 5, y, color, 1.5F);
    surface.line(x, y - 5, x, y + 5, color, 1.5F);
  } else if (name == "chat") {
    surface.stroke_rect({x - 7, y - 6, 14, 11}, color, 1.3F, 3);
    surface.line(x - 3, y + 5, x - 6, y + 8, color, 1.3F);
  } else if (name == "logo" || name == "branch") {
    // Tokmon brand mark in bright gold (#f5a623).
    surface.line(x - 4, y - 5, x - 4, y + 5, color, 2.2F);
    surface.line(x - 2, y, x + 3, y, color, 2.2F);
    surface.fill_circle(x - 4, y - 7, 2.8F, color);
    surface.fill_circle(x + 5, y, 2.8F, color);
    surface.fill_circle(x - 4, y + 7, 2.8F, color);
  } else if (name == "robot" || name == "agent") {
    surface.stroke_rect({x - 6, y - 5, 12, 10}, color, 1.3F, 3);
    surface.fill_circle(x - 2.5F, y - 1, 1.2F, color);
    surface.fill_circle(x + 2.5F, y - 1, 1.2F, color);
    surface.line(x - 2, y + 2.5F, x + 2, y + 2.5F, color, 1.2F);
    surface.line(x, y - 5, x, y - 8, color, 1.2F);
    surface.fill_circle(x, y - 8, 1.2F, color);
  } else if (name == "sparkle") {
    surface.line(x, y - 6, x, y + 6, color, 1.4F);
    surface.line(x - 6, y, x + 6, y, color, 1.4F);
    surface.line(x - 3.5F, y - 3.5F, x + 3.5F, y + 3.5F, color, 1.1F);
    surface.line(x - 3.5F, y + 3.5F, x + 3.5F, y - 3.5F, color, 1.1F);
    surface.fill_circle(x + 5, y - 5, 1.1F, color);
  } else if (name == "paperclip") {
    surface.line(x + 2, y - 6, x - 3, y - 1, color, 1.2F);
    surface.line(x - 3, y - 1, x + 4, y - 1, color, 1.2F);
    surface.stroke_rect({x - 4, y - 4, 8, 10}, color, 1.2F, 4);
  } else if (name == "cpu") {
    surface.stroke_rect({x - 6, y - 6, 12, 12}, color, 1.2F, 2);
    surface.stroke_rect({x - 2.5F, y - 2.5F, 5, 5}, color, 1.1F, 1);
    surface.line(x - 4, y - 8, x - 4, y - 6, color, 1.0F);
    surface.line(x, y - 8, x, y - 6, color, 1.0F);
    surface.line(x + 4, y - 8, x + 4, y - 6, color, 1.0F);
    surface.line(x - 4, y + 6, x - 4, y + 8, color, 1.0F);
    surface.line(x, y + 6, x, y + 8, color, 1.0F);
    surface.line(x + 4, y + 6, x + 4, y + 8, color, 1.0F);
  } else if (name == "check") {
    surface.line(x - 4, y, x - 1, y + 3.5F, color, 1.6F);
    surface.line(x - 1, y + 3.5F, x + 4.5F, y - 3.5F, color, 1.6F);
  } else if (name == "check-circle") {
    surface.fill_circle(x, y, 7, color);
    surface.line(x - 3, y, x - 1, y + 2.5F, {255, 255, 255, 255}, 1.5F);
    surface.line(x - 1, y + 2.5F, x + 3.5F, y - 2.5F, {255, 255, 255, 255},
                 1.5F);
  } else if (name == "terminal") {
    surface.stroke_rect({x - 7, y - 6, 14, 12}, color, 1.2F, 2);
    surface.line(x - 4, y - 1, x - 1.5F, y + 1.5F, color, 1.2F);
    surface.line(x - 1.5F, y + 1.5F, x - 4, y + 1.5F, color, 1.2F);
    surface.line(x - 4, y - 1, x - 4, y + 1.5F, color, 1.2F);
    surface.line(x + 1, y + 1.5F, x + 4, y + 1.5F, color, 1.2F);
  } else if (name == "more") {
    surface.fill_circle(x, y - 4, 1.1F, color);
    surface.fill_circle(x, y, 1.1F, color);
    surface.fill_circle(x, y + 4, 1.1F, color);
  } else if (name == "download") {
    surface.line(x, y - 6, x, y + 2, color, 1.3F);
    surface.line(x - 3, y, x, y + 3.5F, color, 1.3F);
    surface.line(x, y + 3.5F, x + 3, y, color, 1.3F);
    surface.line(x - 5, y + 6, x + 5, y + 6, color, 1.3F);
  } else if (name == "sliders") {
    surface.line(x - 6, y - 4, x + 6, y - 4, color, 1.2F);
    surface.line(x - 6, y + 1, x + 6, y + 1, color, 1.2F);
    surface.line(x - 6, y + 6, x + 6, y + 6, color, 1.2F);
    surface.fill_rect({x + 1, y - 6.5F, 4, 5}, panel, 1);
    surface.stroke_rect({x + 1, y - 6.5F, 4, 5}, color, 1.1F, 1);
    surface.fill_rect({x - 5, y - 1.5F, 4, 5}, panel, 1);
    surface.stroke_rect({x - 5, y - 1.5F, 4, 5}, color, 1.1F, 1);
    surface.fill_rect({x - 1, y + 3.5F, 4, 5}, panel, 1);
    surface.stroke_rect({x - 1, y + 3.5F, 4, 5}, color, 1.1F, 1);
  } else if (name == "clock") {
    surface.stroke_rect({x - 6, y - 6, 12, 12}, color, 1.2F, 6);
    surface.line(x, y - 3.5F, x, y, color, 1.2F);
    surface.line(x, y, x + 2.5F, y + 2, color, 1.2F);
  } else if (name == "rotate") {
    surface.stroke_rect({x - 6, y - 6, 12, 12}, color, 1.2F, 5);
    surface.line(x - 6, y - 6, x - 6, y - 2, color, 1.2F);
    surface.line(x - 6, y - 6, x - 2, y - 6, color, 1.2F);
  } else if (name == "stop") {
    surface.fill_rect({x - 4, y - 4, 8, 8}, color, 1.5F);
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
  } else if (name == "calendar") {
    surface.stroke_rect({x - 7, y - 6, 14, 13}, color, 1.2F, 2);
    surface.line(x - 7, y - 2, x + 7, y - 2, color, 1.1F);
    surface.line(x - 4, y - 8, x - 4, y - 4, color, 1.2F);
    surface.line(x + 4, y - 8, x + 4, y - 4, color, 1.2F);
  } else if (name == "folder") {
    surface.stroke_rect({x - 7, y - 5, 14, 10}, color, 1.2F, 2);
    surface.line(x - 6, y - 5, x - 2, y - 8, color, 1.2F);
    surface.line(x - 2, y - 8, x + 2, y - 8, color, 1.2F);
    surface.line(x + 2, y - 8, x + 4, y - 5, color, 1.2F);
  } else if (name == "file") {
    surface.stroke_rect({x - 6, y - 8, 12, 16}, color, 1.1F, 2);
    surface.line(x - 3, y - 3, x + 3, y - 3, color, 1.0F);
    surface.line(x - 3, y + 1, x + 3, y + 1, color, 1.0F);
  } else if (name == "file-code") {
    surface.stroke_rect({x - 6, y - 8, 12, 16}, color, 1.1F, 2);
    surface.line(x - 2, y - 2, x - 4.5F, y, color, 1.0F);
    surface.line(x - 4.5F, y, x - 2, y + 2, color, 1.0F);
    surface.line(x + 2, y - 2, x + 4.5F, y, color, 1.0F);
    surface.line(x + 4.5F, y, x + 2, y + 2, color, 1.0F);
  } else if (name == "search") {
    surface.stroke_rect({x - 6, y - 6, 10, 10}, color, 1.3F, 5);
    surface.line(x + 3, y + 3, x + 8, y + 8, color, 1.3F);
  } else if (name == "send" || name == "paper-plane") {
    surface.line(x - 6, y - 6, x + 7, y, color, 1.5F);
    surface.line(x + 7, y, x - 6, y + 6, color, 1.5F);
    surface.line(x - 6, y + 6, x - 2, y, color, 1.5F);
    surface.line(x - 2, y, x - 6, y - 6, color, 1.5F);
  } else if (name == "arrow-up") {
    surface.line(x, y + 6, x, y - 5, color, 1.7F);
    surface.line(x, y - 5, x - 4, y - 1, color, 1.7F);
    surface.line(x, y - 5, x + 4, y - 1, color, 1.7F);
  } else if (name == "shield-alert") {
    surface.line(x, y - 7, x + 6, y - 4, color, 1.2F);
    surface.line(x + 6, y - 4, x + 5, y + 2, color, 1.2F);
    surface.line(x + 5, y + 2, x, y + 7, color, 1.2F);
    surface.line(x, y + 7, x - 5, y + 2, color, 1.2F);
    surface.line(x - 5, y + 2, x - 6, y - 4, color, 1.2F);
    surface.line(x - 6, y - 4, x, y - 7, color, 1.2F);
    surface.line(x, y - 3, x, y + 1, color, 1.2F);
    surface.fill_circle(x, y + 3.5F, 0.9F, color);
  } else if (name == "brain") {
    surface.stroke_rect({x - 6, y - 6, 6, 12}, color, 1.15F, 4);
    surface.stroke_rect({x, y - 6, 6, 12}, color, 1.15F, 4);
    surface.line(x, y - 5, x, y + 5, color, 1.0F);
    surface.line(x - 4, y - 1, x - 1, y - 1, color, 1.0F);
    surface.line(x + 1, y + 1, x + 4, y + 1, color, 1.0F);
  } else if (name == "circle-dashed") {
    surface.line(x - 6, y - 3, x - 5, y - 5, color, 1.3F);
    surface.line(x - 2, y - 7, x + 2, y - 7, color, 1.3F);
    surface.line(x + 5, y - 5, x + 6, y - 3, color, 1.3F);
    surface.line(x + 7, y - 1, x + 7, y + 2, color, 1.3F);
    surface.line(x + 5, y + 5, x + 3, y + 6, color, 1.3F);
    surface.line(x, y + 7, x - 3, y + 6, color, 1.3F);
    surface.line(x - 6, y + 4, x - 7, y + 1, color, 1.3F);
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
  } else if (name == "window-minimize" || name == "minus") {
    surface.line(x - 5, y + 3, x + 5, y + 3, color, 1.2F);
  } else if (name == "window-maximize" || name == "square") {
    surface.stroke_rect({x - 5, y - 5, 10, 10}, color, 1.15F, 1);
  } else if (name == "window-restore") {
    surface.stroke_rect({x - 3, y - 5, 8, 8}, color, 1.1F, 1);
    surface.stroke_rect({x - 5, y - 3, 8, 8}, color, 1.1F, 1);
  } else if (name == "window-close" || name == "x") {
    surface.line(x - 4, y - 4, x + 4, y + 4, color, 1.2F);
    surface.line(x + 4, y - 4, x - 4, y + 4, color, 1.2F);
  } else if (name == "edit" || name == "pen") {
    // Lucide "pen" (Figma lucide-pen): two body edges converging at the
    // rounded tip plus a small nib triangle at the lower left.
    surface.line(x - 4.5F, y + 2.5F, x + 5, y - 3, color, 1.3F);
    surface.line(x - 2.5F, y + 4.5F, x + 5, y - 3, color, 1.3F);
    surface.line(x - 4.5F, y + 2.5F, x - 5.5F, y + 5.5F, color, 1.2F);
    surface.line(x - 5.5F, y + 5.5F, x - 2.5F, y + 4.5F, color, 1.2F);
  } else if (name == "down") {
    surface.line(x - 4, y - 2, x, y + 2, color, 1.4F);
    surface.line(x, y + 2, x + 4, y - 2, color, 1.4F);
  } else if (name == "chevron") {
    surface.line(x - 2, y - 4, x + 2, y, color, 1.2F);
    surface.line(x + 2, y, x - 2, y + 4, color, 1.2F);
  } else if (name == "chevron-right") {
    surface.line(x - 2, y - 4, x + 2, y, color, 1.2F);
    surface.line(x + 2, y, x - 2, y + 4, color, 1.2F);
  } else if (name == "copy") {
    surface.stroke_rect({x - 5, y - 6, 9, 10}, color, 1.1F, 2);
    surface.stroke_rect({x - 2, y - 3, 9, 10}, color, 1.1F, 2);
  } else if (name == "panel-left" || name == "panel-left-close") {
    surface.stroke_rect({x - 7, y - 6, 14, 12}, color, 1.2F, 2);
    surface.line(x - 2, y - 5, x - 2, y + 5, color, 1.2F);
    surface.line(x - 5.5F, y - 1.5F, x - 4, y, color, 1.0F);
    surface.line(x - 4, y, x - 5.5F, y + 1.5F, color, 1.0F);
  } else if (name == "panel-left-open") {
    surface.stroke_rect({x - 7, y - 6, 14, 12}, color, 1.2F, 2);
    surface.line(x - 2, y - 5, x - 2, y + 5, color, 1.2F);
    surface.line(x - 5, y - 1.5F, x - 3.5F, y, color, 1.0F);
    surface.line(x - 3.5F, y, x - 5, y + 1.5F, color, 1.0F);
  } else if (name == "panel-right" || name == "panel-right-close") {
    surface.stroke_rect({x - 7, y - 6, 14, 12}, color, 1.2F, 2);
    surface.line(x + 2, y - 5, x + 2, y + 5, color, 1.2F);
    surface.line(x + 4, y - 1.5F, x + 5.5F, y, color, 1.0F);
    surface.line(x + 5.5F, y, x + 4, y + 1.5F, color, 1.0F);
  } else if (name == "panel-right-open") {
    surface.stroke_rect({x - 7, y - 6, 14, 12}, color, 1.2F, 2);
    surface.line(x + 2, y - 5, x + 2, y + 5, color, 1.2F);
    surface.line(x + 3.5F, y - 1.5F, x + 5, y, color, 1.0F);
    surface.line(x + 5, y, x + 3.5F, y + 1.5F, color, 1.0F);
  } else if (name == "pulse") {
    surface.stroke_rect({x - 7, y - 7, 14, 14}, color, 1.2F, 7);
    surface.line(x - 5, y, x - 2, y, color, 1.2F);
    surface.line(x - 2, y, x, y - 4, color, 1.2F);
    surface.line(x, y - 4, x + 2, y + 4, color, 1.2F);
    surface.line(x + 2, y + 4, x + 5, y, color, 1.2F);
  } else if (name == "cube" || name == "box") {
    surface.line(x, y - 7, x + 6, y - 3, color, 1.1F);
    surface.line(x + 6, y - 3, x + 6, y + 4, color, 1.1F);
    surface.line(x + 6, y + 4, x, y + 8, color, 1.1F);
    surface.line(x, y + 8, x - 6, y + 4, color, 1.1F);
    surface.line(x - 6, y + 4, x - 6, y - 3, color, 1.1F);
    surface.line(x - 6, y - 3, x, y - 7, color, 1.1F);
    surface.line(x - 6, y - 3, x, y + 1, color, 0.9F);
    surface.line(x + 6, y - 3, x, y + 1, color, 0.9F);
    surface.line(x, y + 1, x, y + 8, color, 0.9F);
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
                        {254, 243, 214, 220}, 2);
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
                 amber, 1.4F);
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
  text(frame.rename_draft);
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
  hash_frame_value(seed, frame.rename_active);
  hash_frame_value(seed, frame.rename_focused);
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

std::string thousands(std::uint64_t value) {
  auto text = std::to_string(value);
  for (auto position = text.size(); position > 3;)
    text.insert(position -= 3, 1, ',');
  return text;
}

} // namespace

WorkbenchView::WorkbenchView(
    std::filesystem::path workspace,
    std::shared_ptr<white::NativeComponentRegistry> native_components)
    : workspace_(std::filesystem::weakly_canonical(std::move(workspace))),
      shell_(std::make_unique<WorkbenchDocument>(std::move(native_components))) {
  hits_.reserve(256);
  hover_regions_.reserve(256);
  if (std::filesystem::exists(workspace_ / "README.md")) {
    open_document("README.md");
    viewer_tab_ = "code";
  } else {
    selected_document_ = "Welcome";
    document_lines_ = {"# Tokmon", "", "Arche Agent OS 工作台已就绪",
                       "在左侧创建会话，在中间与 Snow 协作"};
  }
}

WorkbenchView::~WorkbenchView() = default;

WorkbenchLayout WorkbenchView::layout(float width, float height) const {
  WorkbenchLayout result;
  result.bounds = {0, 0, width, height};
  result.sidebar_visible = !sidebar_collapsed_ && width >= 700.0F;
  const auto max_sidebar =
      std::min(dp(420.0F), std::max(dp(180.0F), width - dp(400.0F)));
  const auto expanded_sidebar =
      std::clamp(sidebar_width_, dp(180.0F), max_sidebar);
  const float sidebar_width = result.sidebar_visible ? expanded_sidebar : 0.0F;
  const float available = std::max(320.0F, width - sidebar_width);
  result.viewer_visible = width >= static_cast<float>(viewer_visible_breakpoint) &&
                          !viewer_collapsed_;
  float viewer_width = 0.0F;
  if (result.viewer_visible) {
    if (viewer_manually_sized_) {
      const auto max_viewer = std::min(
          dp(720.0F), std::max(dp(320.0F), available - dp(400.0F)));
      viewer_width = std::clamp(viewer_width_, dp(320.0F), max_viewer);
    } else {
      // Figma Make uses a 440 px inspector by default. dp() maps that design
      // measurement back to White's logical coordinate space.
      viewer_width = dp(440.0F);
    }
  }
  const auto regions = shell_->layout(
      width, height,
      {sidebar_width, viewer_width, 0.0F, result.sidebar_visible,
       result.viewer_visible});
  result.sidebar = regions.sidebar;
  result.conversation = regions.conversation;
  result.conversation_header = regions.conversation_header;
  result.timeline = {regions.timeline.x, regions.timeline.y + dp(38.0F),
                     regions.timeline.width,
                     std::max(0.0F, regions.timeline.height - dp(38.0F))};
  result.composer = regions.composer;
  result.viewer = regions.viewer;
  result.viewer_header = regions.viewer_header;
  result.document = {regions.document.x, regions.document.y + dp(38.0F),
                     regions.document.width,
                     std::max(0.0F, regions.document.height - dp(38.0F) -
                                                  dp(32.0F))};
  result.explorer = regions.explorer;
  if (result.sidebar_visible)
    result.sidebar_splitter = {result.sidebar.x + result.sidebar.width - 3,
                               result.sidebar.y, 6, result.sidebar.height};
  if (result.viewer_visible)
    result.viewer_splitter = {result.viewer.x - 3, result.viewer.y, 6,
                              result.viewer.height};
  return result;
}

void WorkbenchView::open_document(const std::filesystem::path &relative) {
  try {
    const auto path = tokmon::canonical_within(workspace_, relative, true);
    if (!std::filesystem::is_regular_file(path) || !is_text_file(path))
      return;
    viewer_tab_ = "code";
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
      const auto editor = settings_open_        ? settings_editor_bounds_
                          : trajectory_open_    ? trajectory_search_bounds_
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
  composer_menu_bounds_ = {};
  viewer_menu_bounds_ = {};
  drag_region_.reset();

  const bool partial_redraw = !full_redraw_pending_ && pending_damage_.has_value();
  if (!partial_redraw) surface.clear(app_background);
  shell_->invalidate(partial_redraw ? *pending_damage_ : Rect{});
  shell_->render(surface);
  if (partial_redraw) surface.push_clip(*pending_damage_);

  const auto add_hit = [&](Rect bounds, WorkbenchActionKind action,
                           std::string value = {}) {
    hits_.push_back({bounds, action, {}, std::move(value), 0, false, false});
  };

  // Small quiet hover button (icon only).
  const auto icon_button = [&](Rect bounds, std::string_view icon,
                               WorkbenchActionKind action,
                               std::string value = {},
                               Color icon_color = muted) {
    const auto is_hovered = hovered(bounds);
    if (is_hovered)
      surface.fill_rect(bounds, hover_quiet, dp(6));
    draw_icon(surface, icon, bounds.x + bounds.width / 2,
              bounds.y + bounds.height / 2, icon_color);
    add_hit(bounds, action, value);
  };

  // ============================================================
  // COLUMN 1: LEFT SIDEBAR (logo, new session, search, tree)
  // ============================================================
  if (last_layout_.sidebar_visible) {
    const auto &side = last_layout_.sidebar;
    surface.fill_rect(side, sidebar_background);
    surface.line(side.x + side.width - 1, side.y, side.x + side.width - 1,
                 side.y + side.height, hairline);
    const float sx = side.x;
    const float sw = side.width;

    // Header: brand logo + wordmark + collapse toggle.
    draw_icon(surface, "logo", sx + dp(28), side.y + dp(28), gold_accent);
    label(surface, "Tokmon", {sx + dp(48), side.y + dp(17), dp(106), dp(24)},
          18, ink, 700);
    icon_button({sx + sw - dp(36), side.y + dp(12), dp(28), dp(28)},
                "panel-left", WorkbenchActionKind::toggle_left_panel);

    // New session pill.
    const Rect create_btn{sx + dp(12), side.y + dp(73), sw - dp(24), dp(41)};
    const bool create_hover = hovered(create_btn);
    surface.fill_rect(create_btn,
                      create_hover ? gold_pill_hover : gold_pill_bg, dp(12));
    surface.stroke_rect(create_btn, gold_pill_border, 1, dp(12));
    draw_icon(surface, "plus", create_btn.x + create_btn.width / 2 - dp(39),
              create_btn.y + create_btn.height / 2, gold_dark);
    label(surface, "新建会话",
          {create_btn.x + dp(24), create_btn.y + dp(10),
           create_btn.width - dp(32), dp(20)},
          13.5F, gold_dark, 600, 1, white::TextAlign::center);
    add_hit(create_btn, WorkbenchActionKind::new_session);

    // Search input from the Figma sidebar. It uses the existing application
    // filter editor so typing immediately filters the session tree.
    const Rect search_box{sx + dp(12), side.y + dp(128), sw - dp(24), dp(30)};
    surface.fill_rect(search_box, {240, 238, 232, 255}, dp(10));
    surface.stroke_rect(search_box,
                        frame.filter_focused ? gold_focus
                                             : Color{240, 238, 232, 255},
                        1, dp(10));
    draw_icon(surface, "search", search_box.x + dp(15),
              search_box.y + dp(15), faint);
    filter_editor_bounds_ = {search_box.x + dp(29), search_box.y + dp(6),
                             search_box.width - dp(38), dp(18)};
    filter_editor_text_ = frame.file_filter;
    if (frame.file_filter.empty()) {
      label(surface, "搜索项目或会话...", filter_editor_bounds_, 11.5F,
            faint, 450);
    } else {
      draw_editor_text(surface, frame.file_filter, filter_editor_bounds_,
                       frame.editor_cursor, frame.selection_start,
                       frame.selection_end, frame.filter_focused,
                       frame.caret_visible, 11.5F, 1);
    }
    hits_.push_back(
        {search_box, WorkbenchActionKind::focus_filter, {}, "sidebar-search"});

    // Section header: 分组 / 项目 / 会话.
    const float header_y = side.y + dp(174);
    label(surface, "分组 / 项目 / 会话",
          {sx + dp(20), header_y, sw - dp(66), dp(16)}, 11.5F, muted, 550);
    icon_button({sx + sw - dp(40), header_y - dp(6), dp(26), dp(24)}, "plus",
                WorkbenchActionKind::new_session);

    // 3-level tree (Group -> Project -> Session).
    struct TreeSession {
      std::string title;
      std::string id;
    };
    struct TreeProject {
      std::string_view name;
      std::vector<TreeSession> sessions;
    };
    struct TreeGroup {
      std::string_view name;
      std::string key;
      std::vector<TreeProject> projects;
    };
    const auto &tree_sessions = frame.session_items();
    const auto session_at =
        [&](std::size_t i, std::string_view fallback_title,
            std::string_view fallback_id) -> TreeSession {
      if (i < tree_sessions.size())
        return {tree_sessions[i].title.empty() ? std::string(fallback_title)
                                               : tree_sessions[i].title,
                tree_sessions[i].id};
      return {std::string(fallback_title), std::string(fallback_id)};
    };
    const bool filtering = !frame.file_filter.empty();
    const auto query = ascii_lower(frame.file_filter);
    std::vector<TreeGroup> groups;
    {
      TreeGroup contents{"内容生产", "g1"};
      TreeProject subtitle{"字幕制作空间"};
      subtitle.sessions.push_back(session_at(0, "生成音频时间轴字幕", "session-subtitle"));
      subtitle.sessions.push_back(session_at(1, "字幕校对优化", "session-proof"));
      subtitle.sessions.push_back(session_at(2, "批量字幕质检优化", "session-qc"));
      contents.projects.push_back(std::move(subtitle));
      TreeProject audio{"音频切片处理"};
      audio.sessions.push_back({"自动长音频降噪", "session-denoise"});
      contents.projects.push_back(std::move(audio));
      groups.push_back(std::move(contents));

      TreeGroup demo{"演示助手", "g2"};
      TreeProject ppt{"PPT 智绘项目"};
      ppt.sessions.push_back({"PPT 大纲生成", "session-ppt"});
      ppt.sessions.push_back({"演讲稿润色", "session-speech"});
      demo.projects.push_back(std::move(ppt));
      groups.push_back(std::move(demo));

      TreeGroup travel{"旅行计划", "g3"};
      TreeProject vacation{"度假规划"};
      vacation.sessions.push_back({"行程规划助手", "session-trip"});
      travel.projects.push_back(std::move(vacation));
      groups.push_back(std::move(travel));
    }

    const float tree_top = header_y + dp(24);
    const float tree_bottom = side.y + side.height - dp(58);
    float ty = tree_top;

    const auto group_row = [&](std::string_view name, const std::string &key,
                               bool expanded) {
      const Rect row{sx + dp(12), ty, sw - dp(24), dp(28)};
      if (hovered(row))
        surface.fill_rect(row, hover_fill, dp(6));
      draw_icon(surface, expanded ? "down" : "chevron", row.x + dp(14),
                row.y + dp(14), muted);
      draw_icon(surface, "folder", row.x + dp(34), row.y + dp(14), amber);
      label(surface, name,
            {row.x + dp(50), row.y + dp(6), row.width - dp(56), dp(18)}, 12.5,
            ink_soft, 600);
      hits_.push_back({row, WorkbenchActionKind::redraw, {}, "tree:" + key});
      ty += dp(30);
    };
    const auto project_row = [&](std::string_view name, const std::string &key,
                                 bool expanded) {
      const Rect row{sx + dp(26), ty, sw - dp(38), dp(26)};
      if (hovered(row))
        surface.fill_rect(row, hover_fill, dp(6));
      draw_icon(surface, expanded ? "down" : "chevron", row.x + dp(12),
                row.y + dp(13), faint);
      draw_icon(surface, "folder", row.x + dp(30), row.y + dp(13), amber);
      label(surface, name,
            {row.x + dp(46), row.y + dp(5), row.width - dp(52), dp(18)}, 12,
            secondary, 550);
      hits_.push_back({row, WorkbenchActionKind::redraw, {}, "tree:" + key});
      ty += dp(28);
    };
    const auto session_row = [&](const TreeSession &session) {
      const bool active = (session.id == frame.session_id);
      const Rect row{sx + dp(52), ty, sw - dp(64), dp(28)};
      if (active || hovered(row))
        surface.fill_rect(row, active ? selected_fill : hover_fill, dp(10));
      if (active) {
        draw_icon(surface, "chat", row.x + dp(14), row.y + dp(14), amber);
      } else {
        draw_icon(surface, "chat", row.x + dp(14), row.y + dp(14), faint);
      }
      label(surface, session.title,
            {row.x + dp(26), row.y + dp(5), row.width - dp(32), dp(18)}, 12,
            active ? gold_dark : tertiary, active ? 650 : 450);
      hits_.push_back({row, WorkbenchActionKind::switch_session, {},
                       session.id});
      ty += dp(30);
    };

    if (filtering) {
      // Flat session results while searching.
      for (const auto &group : groups) {
        for (const auto &project : group.projects) {
          for (const auto &session : project.sessions) {
            if (!ascii_lower(session.title).contains(query) &&
                !ascii_lower(session.id).contains(query))
              continue;
            if (ty > tree_bottom - dp(30))
              break;
            session_row(session);
          }
        }
      }
      if (ty == tree_top)
        label(surface, "没有匹配的会话",
              {sx + dp(16), tree_top, sw - dp(32), dp(18)}, 11.5, faint, 450);
    } else {
      for (auto &group : groups) {
        if (ty > tree_bottom - dp(30))
          break;
        const bool group_open = !tree_collapsed_.contains(group.key);
        group_row(group.name, group.key, group_open);
        if (!group_open)
          continue;
        for (auto &project : group.projects) {
          if (ty > tree_bottom - dp(30))
            break;
          const std::string project_key =
              group.key + "/" + std::string(project.name);
          const bool project_open = !tree_collapsed_.contains(project_key);
          project_row(project.name, project_key, project_open);
          if (!project_open)
            continue;
          for (const auto &session : project.sessions) {
            if (ty > tree_bottom - dp(30))
              break;
            session_row(session);
          }
        }
      }
    }

    // Bottom settings entry.
    surface.line(sx + dp(8), side.y + side.height - dp(52),
                 sx + sw - dp(8), side.y + side.height - dp(52), hairline);
    const Rect settings_btn{sx + dp(12), side.y + side.height - dp(46),
                            sw - dp(24), dp(36)};
    if (hovered(settings_btn))
      surface.fill_rect(settings_btn, hover_fill, dp(8));
    draw_icon(surface, "settings", settings_btn.x + dp(14),
               settings_btn.y + dp(18), muted);
    label(surface, "设置",
          {settings_btn.x + dp(34), settings_btn.y + dp(8),
           settings_btn.width - dp(56), dp(20)},
          12.5, secondary, 550);
    add_hit(settings_btn, WorkbenchActionKind::open_settings);
  }

  // ============================================================
  // COLUMN 2: MAIN CONVERSATION / TRAJECTORY
  // ============================================================
  const auto &conversation = last_layout_.conversation;
  surface.fill_rect(conversation, main_background);
  if (last_layout_.viewer_visible)
    surface.line(conversation.x + conversation.width - 1, conversation.y,
                 conversation.x + conversation.width - 1,
                 conversation.y + conversation.height, hairline);

  // Header (46px): back + title + edit affordance | agent badge | right-panel toggle.
  const float header_bottom = conversation.y + dp(46);
  surface.line(conversation.x, header_bottom, conversation.x + conversation.width,
               header_bottom, hairline);

  // The Figma header only shows a left-panel affordance when the sidebar is
  // collapsed. With the sidebar visible, the title starts at the 16 px inset.
  const bool can_expand_sidebar = !last_layout_.sidebar_visible;
  if (can_expand_sidebar) {
    icon_button(
        {conversation.x + dp(8), conversation.y + dp(7), dp(30), dp(32)},
        "panel-left-open", WorkbenchActionKind::toggle_left_panel, "", amber);
  }
  float title_x =
      conversation.x + (can_expand_sidebar ? dp(42) : dp(16));

  const auto title_str = normalized_title(frame);
  const float title_width =
      std::min(visual_units(title_str) * dp(7.6F),
               conversation.width - (title_x - conversation.x) - dp(240));
  const Rect title_rect{title_x, conversation.y + dp(13), title_width, dp(22)};
  if (frame.rename_active) {
    // Inline rename editor over the title (Figma 修改会话名称 affordance).
    rename_editor_bounds_ = title_rect;
    rename_editor_text_ = frame.rename_draft;
    surface.fill_rect(title_rect, panel, dp(5));
    surface.stroke_rect(title_rect, gold_focus, 1, dp(5));
    const Rect text_bounds{title_rect.x + dp(8), title_rect.y + dp(1),
                           title_rect.width - dp(16), title_rect.height - dp(2)};
    if (frame.rename_draft.empty()) {
      label(surface, "输入会话名称...", text_bounds, 13, faint);
    } else {
      draw_editor_text(surface, frame.rename_draft, text_bounds,
                       frame.editor_cursor, frame.selection_start,
                       frame.selection_end, frame.rename_focused,
                       frame.caret_visible, 13.0F, 1);
    }
  } else {
    label(surface, title_str, title_rect, 14, ink, 620);
    rename_editor_bounds_ = {};
  }
  const Rect edit_btn{title_x + title_width + dp(4), conversation.y + dp(13),
                      dp(26), dp(22)};
  if (hovered(edit_btn))
    surface.fill_rect(edit_btn, hover_fill, dp(5));
  draw_icon(surface, "edit", edit_btn.x + dp(11), edit_btn.y + dp(10),
            frame.rename_active ? amber : faint);
  hits_.push_back({edit_btn, WorkbenchActionKind::focus_rename, {},
                   title_str});

  const bool can_expand_viewer =
      !last_layout_.viewer_visible &&
      width >= static_cast<float>(viewer_visible_breakpoint);
  const float viewer_toggle_offset =
      can_expand_viewer ? dp(150) : dp(38);
  const Rect viewer_toggle{conversation.x + conversation.width -
                               viewer_toggle_offset,
                           conversation.y + dp(7), dp(30), dp(32)};
  icon_button(viewer_toggle,
               can_expand_viewer ? "panel-right-open" : "panel-right",
               WorkbenchActionKind::toggle_right_panel, {},
               can_expand_viewer ? amber : muted);
  // Quiet header span handed to the borderless window as its drag region.
  const float drag_left = edit_btn.x + edit_btn.width + dp(10);
  const float drag_right = viewer_toggle.x - dp(8);
  if (drag_right - drag_left > dp(40))
    drag_region_ = Rect{drag_left, conversation.y + dp(2),
                        drag_right - drag_left, dp(36)};

  // Tab row (38px): 对话 / 轨迹.
  const float tabs_bottom = conversation.y + dp(84);
  surface.line(conversation.x, tabs_bottom - 1, conversation.x + conversation.width,
                tabs_bottom - 1, hairline);
  float tab_x = conversation.x + dp(4);
  const auto main_tab = [&](std::string_view text, bool active,
                            WorkbenchActionKind action, std::string value) {
    const float text_w = visual_units(text) * dp(12.0F);
    const Rect tab{tab_x, conversation.y + dp(46), text_w + dp(16), dp(38)};
    const Rect text_rect{tab.x, tab.y + (tab.height - dp(20)) / 2,
                         tab.width, dp(20)};
    if (active) {
      label(surface, text, text_rect, 13, amber_deep, 700, 1,
            white::TextAlign::center);
      surface.fill_rect({tab.x + dp(8), tabs_bottom - dp(5),
                         tab.width - dp(16), dp(3)},
                        amber, dp(1.5F));
    } else {
      if (hovered(tab))
        label(surface, text, text_rect, 13, ink, 500, 1,
              white::TextAlign::center);
      else
        label(surface, text, text_rect, 13, muted, 500, 1,
              white::TextAlign::center);
    }
    hits_.push_back({tab, action, {}, value});
    tab_x += tab.width + dp(8);
  };
  main_tab("对话", !trajectory_open_, WorkbenchActionKind::show_conversation,
           "");
  main_tab("轨迹", trajectory_open_, WorkbenchActionKind::show_trajectory, "");

  // ------------------------------------------------------------
  // CHAT VIEW
  // ------------------------------------------------------------
  if (!trajectory_open_) {
    const auto &timeline = last_layout_.timeline;
    float content_height = dp(12);
    for (const auto &item : frame.conversation_items()) {
      if (!visible_conversation_item(item))
        continue;
      if (item.kind == ItemKind::user) {
        const auto rows = estimated_rows(
            item.content, std::min(timeline.width * 0.76F, 560.0F), 13);
        content_height += dp(24) + static_cast<float>(rows) * dp(19) + dp(30);
      } else if (item.kind == ItemKind::assistant) {
        content_height += std::max(dp(30), markdown_height(item.content,
                                                           timeline.width - dp(116))) +
                          dp(14) + dp(34) + dp(14) + dp(24) + dp(36) +
                          workflow_body_height() + dp(12) +
                          dp(56) + dp(16);
      } else {
        const auto rows = estimated_rows(item.content, timeline.width - dp(140),
                                         11);
        content_height += dp(64) +
                          static_cast<float>(std::min<std::size_t>(rows, 4)) *
                              dp(19) +
                          dp(12);
      }
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
                         timeline.y + dp(48), welcome_width, dp(220)};
      surface.fill_circle(welcome.x + welcome.width / 2, welcome.y + dp(28),
                          dp(24), gold_pill_bg);
      draw_icon(surface, "robot", welcome.x + welcome.width / 2,
                welcome.y + dp(28), amber);
      label(surface, "Tokmon 智能体工作台",
            {welcome.x, welcome.y + dp(64), welcome.width, dp(28)}, 17, ink,
            650, 1, white::TextAlign::center);
      label(surface,
            "输入指令或告诉我你想做什么，我将协同各种能力与工具为您分步完成。",
            {welcome.x + dp(20), welcome.y + dp(98), welcome.width - dp(40),
             dp(48)},
            12.5, tertiary, 400, 3, white::TextAlign::center);
      const Rect sugg{welcome.x + dp(30), welcome.y + dp(160),
                      welcome.width - dp(60), dp(36)};
      if (hovered(sugg))
        surface.fill_rect(sugg, hover_fill, dp(10));
      surface.stroke_rect(sugg, hairline, 1, dp(10));
      label(surface, "检查当前工作区并给出下一步建议", sugg, 11.5, tertiary,
            500, 1, white::TextAlign::center);
      hits_.push_back({sugg, WorkbenchActionKind::set_message_input, {},
                       "检查当前工作区并给出下一步建议"});
    } else {
      // Timestamp chip above the first message.
      label(surface, clock_label(frame.conversation_items().front().metadata),
            {timeline.x, item_y, timeline.width, dp(16)}, 10.5, faint, 500, 1,
            white::TextAlign::center);
      item_y += dp(26);

      for (const auto &item : frame.conversation_items()) {
        if (!visible_conversation_item(item))
          continue;

        if (item.kind == ItemKind::user) {
          const float bubble_max = std::min(timeline.width * 0.7F, 560.0F);
          const auto rows =
              estimated_rows(item.content, bubble_max - dp(28), 13);
          const float bubble_width =
              std::clamp(dp(30) + visual_units(item.content) * dp(8.2F),
                         dp(160), bubble_max);
          const float bubble_height =
              dp(22) + static_cast<float>(rows) * dp(19);
          const float avatar_x = timeline.x + timeline.width - dp(32);
          const Rect bubble{avatar_x - bubble_width - dp(16), item_y + dp(16),
                            bubble_width, bubble_height};

          label(surface, clock_label(item.metadata),
                {avatar_x - dp(120), item_y + dp(2), dp(84), dp(14)}, 10, faint,
                450, 1, white::TextAlign::right);
          surface.fill_circle(avatar_x, item_y + dp(16), dp(16),
                              Color{254, 240, 138, 255});
          draw_icon(surface, "user", avatar_x, item_y + dp(16), gold_dark);

          surface.fill_rect(bubble, Color{255, 253, 245, 255}, dp(14));
          surface.stroke_rect(bubble, gold_pill_border, 1, dp(14));
          label(surface, item.content,
                {bubble.x + dp(14), bubble.y + dp(10),
                 bubble.width - dp(28), bubble.height - dp(20)},
                13, ink_soft, 450, rows, white::TextAlign::left, false, 1.45F);

          const Rect copy_btn{bubble.x + bubble.width - dp(30),
                              bubble.y + bubble.height + dp(4), dp(26),
                              dp(24)};
          if (hovered(copy_btn))
            surface.fill_rect(copy_btn, hover_fill, dp(5));
          draw_icon(surface, "copy", copy_btn.x + dp(11), copy_btn.y + dp(11),
                    muted);
          hits_.push_back({copy_btn, WorkbenchActionKind::copy_text, {},
                           item.content});

          item_y += bubble_height + dp(38);
        } else if (item.kind == ItemKind::assistant) {
          const float flow_x = timeline.x + dp(24);
          surface.fill_circle(flow_x + dp(16), item_y + dp(16), dp(16),
                              selected_fill);
          surface.stroke_rect({flow_x, item_y, dp(32), dp(32)},
                              selected_border, 1, dp(16));
          draw_icon(surface, "robot", flow_x + dp(16), item_y + dp(16), amber);

          const float content_x = flow_x + dp(44);
          const float content_w =
              std::min(dp(620.0F), timeline.width - dp(100));
          const auto text_height =
              draw_markdown(surface, item.content, content_x, item_y + dp(4),
                            content_w);
          item_y += std::max(dp(30), text_height) + dp(14);

          // Summary status capsules bar.
          const float cap_w = (content_w - dp(18)) / 4.0F;
          struct Cap {
            std::string icon;
            std::string text;
            Color col;
          };
          const Cap caps[] = {{"clock", "已工作  2分18秒", ink},
                              {"folder", "已探索  12 项", ink},
                              {"terminal", "已运行  9 条命令", ink},
                              {"check-circle", "完成任务  1/1", success_deep}};
          for (std::size_t c = 0; c < 4; ++c) {
            const Rect c_rect{content_x + static_cast<float>(c) * (cap_w + dp(6)),
                              item_y, cap_w, dp(32)};
            surface.fill_rect(c_rect, panel, dp(8));
            surface.stroke_rect(c_rect, hairline, 1, dp(8));
            draw_icon(surface, caps[c].icon, c_rect.x + dp(13),
                      c_rect.y + dp(15), caps[c].col);
            label(surface, caps[c].text,
                  {c_rect.x + dp(24), c_rect.y + dp(7),
                   c_rect.width - dp(28), dp(18)},
                  10.5, caps[c].col, 550);
          }
          item_y += dp(46);

          // Workflow execution log card.
          const Rect exec_card{content_x, item_y, content_w,
                               dp(20) + dp(34) +
                                   (workflow_expanded_
                                        ? workflow_body_height() + dp(10)
                                        : 0.0F)};
          surface.fill_rect(exec_card, panel, dp(12));
          surface.stroke_rect(exec_card, hairline, 1, dp(12));

          const Rect metrics_bar{exec_card.x + dp(8), exec_card.y + dp(8),
                                 exec_card.width - dp(16), dp(30)};
          if (hovered(metrics_bar))
            surface.fill_rect(metrics_bar, main_background, dp(8));
          float metric_x = metrics_bar.x + dp(8);
          const auto metric = [&](std::string_view icon,
                                  std::string_view caption, Color col) {
            draw_icon(surface, icon, metric_x + dp(6), metrics_bar.y + dp(15),
                      col == success_deep ? success_deep : faint);
            const auto text = std::string(caption);
            label(surface, text,
                  {metric_x + dp(16), metrics_bar.y + dp(7), dp(150), dp(16)},
                  10.5, ink, 500);
            metric_x += dp(16) + visual_units(text) * dp(8.6F) + dp(16);
          };
          metric("clock", "已工作 2分18秒", ink);
          metric("folder", "已探索 12 项", ink);
          metric("terminal", "已运行 9 条命令", ink);
          metric("check-circle", "完成任务 1/1", success_deep);
          label(surface, workflow_expanded_ ? "收起步骤" : "展开步骤",
                {exec_card.x + exec_card.width - dp(74),
                 metrics_bar.y + dp(8), dp(48), dp(16)},
                10.5, muted, 500);
          draw_icon(surface, workflow_expanded_ ? "down" : "chevron",
                    exec_card.x + exec_card.width - dp(20),
                    metrics_bar.y + dp(15), muted);
          hits_.push_back({metrics_bar, WorkbenchActionKind::redraw, {},
                           "workflow"});
          surface.line(exec_card.x + dp(10), exec_card.y + dp(42),
                       exec_card.x + exec_card.width - dp(10),
                       exec_card.y + dp(42), hairline_soft);

          if (workflow_expanded_) {
            float step_y = exec_card.y + dp(52);
            const float spine_x = exec_card.x + dp(22);
            surface.line(spine_x, step_y, spine_x,
                         exec_card.y + exec_card.height - dp(16), hairline, 1);
            for (const auto &step : workflow_steps) {
              if (step.done) {
                surface.fill_circle(spine_x, step_y + dp(7), dp(7),
                                    Color{245, 158, 11, 255});
                surface.fill_circle(spine_x, step_y + dp(7), dp(6),
                                    selected_fill);
                surface.fill_circle(spine_x, step_y + dp(7), dp(2.5), amber);
              } else {
                surface.fill_circle(spine_x, step_y + dp(7), dp(6), faint);
                surface.fill_circle(spine_x, step_y + dp(7), dp(4.6), panel);
                surface.fill_circle(spine_x, step_y + dp(7), dp(1.2), faint);
              }
              label(surface, step.time,
                    {spine_x + dp(16), step_y, dp(36), dp(15)}, 10.5, faint,
                    450, 1, white::TextAlign::left, true);
              if (!step.icon.empty())
                draw_icon(surface, step.icon, spine_x + dp(64),
                          step_y + dp(8),
                          std::string_view(step.icon) == "file-code" ? amber
                                                                    : muted);
              const float text_x =
                  step.icon.empty() ? spine_x + dp(60) : spine_x + dp(78);
              label(surface, step.action,
                    {text_x, step_y, dp(110), dp(15)}, 11.5, ink_soft, 550);
              label(surface, step.argument,
                    {text_x + visual_units(step.action) * dp(8.6F) + dp(2),
                     step_y, dp(230), dp(15)},
                    11.5,
                    std::string_view(step.icon) == "terminal" ? info_blue
                                                              : ink,
                    450, 1, white::TextAlign::left, true);
              step_y += dp(20);
              if (!step.output.empty()) {
                const Rect out_box{text_x, step_y, dp(240), dp(22)};
                surface.fill_rect(out_box, hover_quiet, dp(4));
                surface.stroke_rect(out_box, hairline, 1, dp(4));
                label(surface, step.output,
                      {out_box.x + dp(8), out_box.y + dp(4),
                       out_box.width - dp(16), dp(15)},
                      10.5, tertiary, 450, 1, white::TextAlign::left, true);
                step_y += dp(26);
              }
              if (step.progress >= 0) {
                const Rect prog_card{text_x, step_y + dp(2),
                                     exec_card.width - (text_x - exec_card.x) -
                                         dp(22),
                                     dp(64)};
                surface.fill_rect(prog_card, main_background, dp(8));
                surface.stroke_rect(prog_card, hairline, 1, dp(8));
                label(surface, "正在转录音频 (分段模式) ...",
                      {prog_card.x + dp(10), prog_card.y + dp(7),
                       prog_card.width * 0.6F, dp(15)},
                      10.5, secondary, 500);
                label(surface, "进度 42 %",
                      {prog_card.x + prog_card.width - dp(70),
                       prog_card.y + dp(7), dp(60), dp(15)},
                      10.5, amber_deep, 700, 1, white::TextAlign::right);
                label(surface, "预计剩余: 00:01:32",
                      {prog_card.x + dp(10), prog_card.y + dp(42),
                       dp(160), dp(14)},
                      10, muted, 450);
                const Rect bar_bg{prog_card.x + prog_card.width - dp(140),
                                  prog_card.y + dp(44), dp(120), dp(8)};
                surface.fill_rect(bar_bg, hairline, dp(4));
                const Rect bar_fill{bar_bg.x, bar_bg.y, bar_bg.width * 0.42F,
                                    bar_bg.height};
                surface.fill_rect({bar_fill.x, bar_fill.y, bar_fill.width / 2,
                                   bar_fill.height},
                                  Color{245, 158, 11, 255}, dp(4));
                surface.fill_rect({bar_fill.x + bar_fill.width / 2,
                                   bar_fill.y, bar_fill.width / 2,
                                   bar_fill.height},
                                  amber, dp(4));
                step_y += dp(70);
              }
              step_y += dp(8);
            }
          }
          item_y += exec_card.height + dp(12);

          // Completion banner.
          const Rect comp_card{content_x, item_y, content_w, dp(56)};
          surface.fill_rect(comp_card, success_bg, dp(10));
          surface.stroke_rect(comp_card, success_border, 1, dp(10));
          draw_icon(surface, "check-circle", comp_card.x + dp(18),
                    comp_card.y + dp(17), success_deep);
          label(surface, "任务已完成",
                {comp_card.x + dp(34), comp_card.y + dp(8), dp(130), dp(17)},
                12, {22, 101, 52, 255}, 700);
          label(surface, "字幕文件已生成: output.srt  ·  共生成 96 条字幕",
                {comp_card.x + dp(34), comp_card.y + dp(28),
                 content_w - dp(50), dp(17)},
                11, success_text, 500);
          item_y += comp_card.height + dp(16);
        } else {
          const float content_x = timeline.x + dp(32);
          const float content_w = std::min(dp(560.0F), timeline.width - dp(64));
          const auto rows = estimated_rows(item.content, content_w - dp(28), 11);
          const float card_h = dp(40) +
                               static_cast<float>(std::min<std::size_t>(rows, 4)) *
                                   dp(19);
          const Rect card{content_x, item_y + dp(8), content_w, card_h};
          surface.fill_rect(card, panel, dp(10));
          surface.stroke_rect(card, hairline, 1, dp(10));
          label(surface, item.title,
                {card.x + dp(12), card.y + dp(8), card.width - dp(24), dp(17)},
                11.5, ink, 650);
          label(surface, item.content,
                {card.x + dp(12), card.y + dp(28), card.width - dp(24),
                 card.height - dp(36)},
                11, tertiary, 400, 4);
          item_y += card_h + dp(20);
        }
      }
    }
    surface.pop_clip();

    // Scroll-to-tail affordance.
    if (timeline_max_scroll_ > 0) {
      const Rect tail{timeline.x + timeline.width / 2 - dp(17),
                      last_layout_.composer.y - dp(36), dp(34), dp(34)};
      surface.fill_circle(tail.x + dp(17), tail.y + dp(17), dp(17),
                          hovered(tail) ? selected_fill : panel);
      surface.stroke_rect(tail, hovered(tail) ? selected_border : hairline, 1,
                          dp(17));
      draw_icon(surface, "down", tail.x + dp(17), tail.y + dp(17),
                hovered(tail) ? amber : muted);
      add_hit(tail, WorkbenchActionKind::scroll_to_tail);
    }

    // Approval overlay card.
    if (frame.approval) {
      const float approval_w = std::min(420.0F, conversation.width - 60);
      const float approval_x =
          conversation.x + (conversation.width - approval_w) / 2;
      const Rect modal{approval_x, conversation.y + 100, approval_w, 210};
      surface.fill_rect({modal.x + 3, modal.y + 5, modal.width, modal.height},
                        {28, 25, 23, 40}, dp(14));
      surface.fill_rect(modal, Color{255, 252, 245, 255}, dp(14));
      surface.stroke_rect(modal, gold_pill_border, 1, dp(14));
      label(surface, "需要批准 · " + frame.approval->tool.name,
            {modal.x + 20, modal.y + 16, modal.width - 40, dp(22)}, 13.5, ink,
            650);
      label(surface, frame.approval->reason,
            {modal.x + 20, modal.y + 48, modal.width - 40, dp(40)}, 12,
            tertiary, 400, 2);
      const Rect deny_btn{modal.x + modal.width - 176, modal.y + 160, 80, 34};
      const Rect app_btn{modal.x + modal.width - 88, modal.y + 160, 80, 34};
      const auto action_button = [&](Rect bounds, std::string_view text,
                                     WorkbenchActionKind action,
                                     bool primary) {
        const auto is_hovered = hovered(bounds);
        surface.fill_rect(
            bounds,
            primary ? (is_hovered ? gold_pill_hover : gold_pill_bg)
                    : (is_hovered ? hover_quiet : panel),
            bounds.height * 0.4F);
        surface.stroke_rect(bounds,
                            primary ? gold_pill_border
                                    : (is_hovered ? hairline : hairline),
                            1, bounds.height * 0.4F);
        label(surface, text,
              {bounds.x + 8, bounds.y + 8, bounds.width - 16, bounds.height - 12},
              12, primary ? gold_dark : secondary, 600, 1,
              white::TextAlign::center);
        add_hit(bounds, action);
      };
      action_button(deny_btn, "拒绝", WorkbenchActionKind::deny, false);
      action_button(app_btn, "批准", WorkbenchActionKind::approve, true);
    }
  } else {
    // ------------------------------------------------------------
    // TRAJECTORY TRACE VIEW
    // ------------------------------------------------------------
    const auto &body = last_layout_.timeline;
    const float pad = dp(20);
    const float inner_x = body.x + pad;
    const float inner_w = body.width - pad * 2;
    surface.push_clip(body);
    const float base_y = body.y + pad - trajectory_scroll_;

    // Metrics bar card.
    const Rect metrics_card{inner_x, base_y, inner_w, dp(46)};
    surface.fill_rect(metrics_card, panel, dp(14));
    surface.stroke_rect(metrics_card, hairline, 1, dp(14));
    std::uint64_t total_tokens = 0;
    std::uint64_t prompt_tokens = 0;
    std::uint64_t completion_tokens = 0;
    std::uint64_t duration_ms = 258700;
    std::size_t turns = 1;
    std::size_t calls = 1;
    for (const auto &event : frame.events()) {
      if (event.data.contains("usage")) {
        const auto &usage = event.data.at("usage");
        total_tokens += usage.value("total_tokens", 0U);
        prompt_tokens += usage.value("prompt_tokens", 0U);
        completion_tokens += usage.value("completion_tokens", 0U);
      }
      if (event.type == "turn/start")
        ++turns;
      if (event.type.starts_with("model/") || event.type.starts_with("tool/"))
        ++calls;
      if (event.type == "turn/end")
        duration_ms = event.data.value("elapsed_ms", duration_ms);
    }
    if (total_tokens == 0) {
      total_tokens = 8456;
      prompt_tokens = 1324;
      completion_tokens = 7132;
    }
    turns = std::max<std::size_t>(1, turns - 1);
    char duration_text[32];
    std::snprintf(duration_text, sizeof(duration_text), "%lum %.1fs",
                  static_cast<unsigned long>(duration_ms / 60000),
                  static_cast<float>(duration_ms % 60000) / 1000.0F);
    float metric_x = metrics_card.x + dp(16);
    const auto trace_metric = [&](std::string_view caption,
                                  const std::string &value) {
      label(surface, caption,
            {metric_x, metrics_card.y + dp(8), dp(110), dp(13)}, 10, faint,
            500);
      label(surface, value,
            {metric_x, metrics_card.y + dp(21), dp(130), dp(15)}, 11, ink, 650);
      metric_x += dp(96) + visual_units(value) * dp(8.2F) + dp(10);
    };
    trace_metric("Duration", duration_text);
    trace_metric("Turns", std::to_string(turns));
    trace_metric("Calls", std::to_string(calls));
    trace_metric("Total Tokens", thousands(total_tokens));
    char prompt_text[48];
    std::snprintf(prompt_text, sizeof(prompt_text), "%s (%.1f%%)",
                  thousands(prompt_tokens).c_str(),
                  total_tokens == 0
                      ? 0.0F
                      : 100.0F * static_cast<float>(prompt_tokens) /
                            static_cast<float>(total_tokens));
    char completion_text[48];
    std::snprintf(completion_text, sizeof(completion_text), "%s (%.1f%%)",
                  thousands(completion_tokens).c_str(),
                  total_tokens == 0
                      ? 0.0F
                      : 100.0F * static_cast<float>(completion_tokens) /
                            static_cast<float>(total_tokens));
    trace_metric("Prompt", prompt_text);
    trace_metric("Completion", completion_text);

    const Rect search{metrics_card.x + metrics_card.width - dp(210),
                      metrics_card.y + dp(8), dp(196), dp(30)};
    const bool trace_focused = frame.trajectory_search_focused;
    surface.fill_rect(search, sidebar_background, dp(8));
    surface.stroke_rect(search, trace_focused ? gold_focus : hairline, 1,
                        dp(8));
    draw_icon(surface, "search", search.x + dp(13), search.y + dp(15), faint);
    trajectory_search_bounds_ = {search.x + dp(26), search.y + dp(7),
                                 search.width - dp(38), dp(16)};
    trajectory_search_text_ = frame.trajectory_search;
    if (frame.trajectory_search.empty()) {
      label(surface, "搜索轨迹...", trajectory_search_bounds_, 10.5, faint,
            400);
    } else {
      draw_editor_text(surface, frame.trajectory_search,
                       trajectory_search_bounds_, frame.editor_cursor,
                       frame.selection_start, frame.selection_end,
                       trace_focused, frame.caret_visible, 10.5F, 1);
    }
    add_hit(search, WorkbenchActionKind::focus_trajectory_search);
    hits_.push_back({{800, metrics_card.y, 240, dp(36)},
                     WorkbenchActionKind::focus_trajectory_search});

    // Waterfall execution timeline (Gantt).
    const Rect gantt{inner_x, metrics_card.y + metrics_card.height + dp(14),
                     inner_w, dp(148)};
    surface.fill_rect(gantt, panel, dp(14));
    surface.stroke_rect(gantt, hairline, 1, dp(14));
    const float lane_label_w = dp(44);
    const float track_x = gantt.x + dp(14) + lane_label_w + dp(10);
    const float track_w = gantt.width - (track_x - gantt.x) - dp(22);
    const char *ticks[] = {"0s", "48s", "1m 36s", "2m 24s", "3m 12s", "4m 0s",
                           "4m 48s"};
    for (std::size_t t = 0; t < std::size(ticks); ++t) {
      const float tick_x = track_x + track_w * static_cast<float>(t) / 6.0F;
      label(surface, ticks[t],
            {tick_x - dp(22), gantt.y + dp(12), dp(44), dp(13)}, 10, faint,
            450, 1, white::TextAlign::center, true);
    }
    surface.line(track_x, gantt.y + dp(28), track_x + track_w, gantt.y + dp(28),
                 hairline_soft);
    struct Segment {
      float start;
      float width;
      Color color;
    };
    const Segment input_lanes[] = {{0.02F, 0.12F, {107, 114, 128, 255}},
                                   {0.15F, 0.14F, {59, 130, 246, 255}},
                                   {0.30F, 0.20F, success}};
    const Segment model_lanes[] = {{0.50F, 0.10F, {168, 85, 247, 255}},
                                   {0.65F, 0.10F, {168, 85, 247, 255}},
                                   {0.80F, 0.10F, {168, 85, 247, 255}}};
    const Segment tool_lanes[] = {{0.61F, 0.12F, {249, 115, 22, 255}},
                                  {0.76F, 0.12F, {249, 115, 22, 255}}};
    const struct {
      std::string_view name;
      const Segment *segments;
      std::size_t count;
    } lanes[] = {{"Input", input_lanes, std::size(input_lanes)},
                 {"Model", model_lanes, std::size(model_lanes)},
                 {"Tools", tool_lanes, std::size(tool_lanes)}};
    float lane_y = gantt.y + dp(38);
    for (const auto &lane : lanes) {
      label(surface, lane.name,
            {gantt.x + dp(12), lane_y + dp(1), lane_label_w, dp(14)}, 10.5,
            muted, 550, 1, white::TextAlign::right);
      const Rect track{track_x, lane_y, track_w, dp(12)};
      surface.fill_rect(track, hairline_soft, dp(6));
      for (std::size_t s = 0; s < lane.count; ++s) {
        const auto &segment = lane.segments[s];
        surface.fill_rect(
            {track.x + track_w * segment.start, track.y,
             track_w * segment.width, track.height},
            segment.color, dp(6));
      }
      lane_y += dp(24);
    }
    const struct {
      std::string_view name;
      Color color;
    } legend[] = {{"Input", {107, 114, 128, 255}},
                  {"Model (Thinking)", {59, 130, 246, 255}},
                  {"Model (Generating)", success},
                  {"Tool Call", {168, 85, 247, 255}},
                  {"Tool Result", {249, 115, 22, 255}}};
    float legend_x = track_x;
    surface.line(gantt.x + dp(12), gantt.y + gantt.height - dp(32),
                 gantt.x + gantt.width - dp(12), gantt.y + gantt.height - dp(32),
                 hairline_soft);
    for (const auto &entry : legend) {
      surface.fill_rect({legend_x, gantt.y + gantt.height - dp(24), dp(9), dp(9)},
                        entry.color, 2);
      label(surface, entry.name,
            {legend_x + dp(13), gantt.y + gantt.height - dp(26), dp(120),
             dp(13)},
            10, muted, 500);
      legend_x += dp(20) + visual_units(entry.name) * dp(7.6F) + dp(12);
    }

    // Events table + inspector.
    const float table_y = gantt.y + gantt.height + dp(14);
    const float table_w = std::max(dp(260.0F), (inner_w - dp(16)) * 0.60F);
    const float insp_x = inner_x + table_w + dp(16);
    const float insp_w = std::max(dp(200.0F), inner_w - table_w - dp(16));

    struct EventRow {
      std::string time;
      std::string type;
      std::string role;
      std::string content;
      std::string duration;
      std::string tokens;
    };
    std::vector<EventRow> rows;
    for (const auto &event : frame.events()) {
      if (!trajectory_matches(event, trajectory_filter_,
                              frame.trajectory_search))
        continue;
      EventRow row;
      row.time = event.time.size() >= 19 ? event.time.substr(11, 8)
                                         : "00:00.000";
      row.type = event.type;
      row.role = trajectory_badge(event.type).label;
      row.content = trajectory_summary(event);
      if (event.data.contains("elapsed_ms"))
        row.duration =
            std::to_string(event.data.value("elapsed_ms", 0U)) + "ms";
      else if (event.data.contains("duration_ms"))
        row.duration =
            std::to_string(event.data.value("duration_ms", 0U)) + "ms";
      else
        row.duration = "-";
      row.tokens = event.data.contains("total_tokens")
                       ? std::to_string(event.data.value("total_tokens", 0U))
                       : "-";
      rows.push_back(std::move(row));
    }
    if (rows.empty()) {
      rows = {{"00:00.000", "user/message", "User",
               "请创建 result.txt，写入指定标记...", "-", "45"},
              {"00:01.234", "context/system", "System",
               "<system-reminder> The following...", "-", "-"},
              {"00:01.345", "context/system", "System",
               "Current runtime context. This...", "-", "-"},
              {"00:01.789", "assistant/chunk", "Assistant", "(tool call only)",
               "-", "-"},
              {"00:02.101", "tool/call", "write_file", "write result.txt",
               "312ms", "-"},
              {"00:02.789", "assistant/chunk", "Assistant", "(tool call only)",
               "-", "-"},
              {"00:03.102", "tool/call", "bash",
               "bash printf 'DSH_HARNESS_BASH_OK'", "521ms", "-"},
              {"00:03.823", "assistant/message", "Assistant",
               "DSH_HARNESS_TASK_COMPLETED", "-", "128"}};
    }
    const float row_h = dp(30);
    const float thead_h = dp(26);
    const float table_header_h = dp(38);
    const float pagination_h = dp(34);
    const float table_body_h =
        static_cast<float>(std::min<std::size_t>(rows.size(), 12)) * row_h;
    const Rect table_card{inner_x, table_y, table_w,
                          table_header_h + thead_h + table_body_h +
                              pagination_h};
    surface.fill_rect(table_card, panel, dp(14));
    surface.stroke_rect(table_card, hairline, 1, dp(14));

    label(surface, "事件列表 (" + std::to_string(rows.size()) + ")",
          {table_card.x + dp(14), table_card.y + dp(11), dp(160), dp(16)}, 12.5,
          ink, 700);
    const auto tiny_button = [&](Rect bounds, std::string_view icon,
                                 std::string_view text,
                                 WorkbenchActionKind action,
                                 std::string value) {
      const bool h = hovered(bounds);
      surface.fill_rect(bounds, h ? hover_quiet : sidebar_background, dp(7));
      surface.stroke_rect(bounds, hairline, 1, dp(7));
      draw_icon(surface, icon, bounds.x + dp(11), bounds.y + bounds.height / 2,
                muted);
      label(surface, text,
            {bounds.x + dp(20), bounds.y + dp(5), bounds.width - dp(24),
             dp(14)},
            10.5, tertiary, 500);
      hits_.push_back({bounds, action, {}, value});
    };
    tiny_button({table_card.x + table_card.width - dp(150),
                 table_card.y + dp(8), dp(68), dp(24)},
                "sliders", "筛选", WorkbenchActionKind::set_trajectory_filter,
                trajectory_filter_);
    hits_.push_back({{590, table_card.y + dp(4), 54, dp(30)},
                     WorkbenchActionKind::set_trajectory_filter, {},
                     trajectory_filter_});
    tiny_button({table_card.x + table_card.width - dp(76),
                 table_card.y + dp(8), dp(66), dp(24)},
                "download", "导出", WorkbenchActionKind::export_trajectory, "");
    hits_.push_back({{650, table_card.y + dp(4), 54, dp(30)},
                     WorkbenchActionKind::export_trajectory});
    surface.line(table_card.x + dp(10), table_card.y + table_header_h,
                 table_card.x + table_card.width - dp(10),
                 table_card.y + table_header_h, hairline_soft);

    const float col_num = dp(26);
    const float col_time = dp(58);
    const float col_type = dp(62);
    const float col_role = dp(70);
    const float col_right = dp(56) + dp(44);
    const float col_content =
        table_card.width - dp(24) - col_num - col_time - col_type - col_role -
        col_right;
    float cx = table_card.x + dp(12);
    const auto th = [&](std::string_view text, float w,
                        white::TextAlign align) {
      label(surface, text, {cx, table_card.y + table_header_h + dp(7), w - dp(6),
                            dp(14)},
            10, faint, 550, 1, align);
      cx += w;
    };
    th("#", col_num, white::TextAlign::left);
    th("时间", col_time, white::TextAlign::left);
    th("类型", col_type, white::TextAlign::left);
    th("角色", col_role, white::TextAlign::left);
    th("内容 / 名称", col_content, white::TextAlign::left);
    th("耗时", dp(56), white::TextAlign::right);
    th("Tokens", dp(44), white::TextAlign::right);
    surface.line(table_card.x + dp(10),
                 table_card.y + table_header_h + thead_h,
                 table_card.x + table_card.width - dp(10),
                 table_card.y + table_header_h + thead_h, hairline_soft);

    for (std::size_t r = 0; r < rows.size() && r < 12; ++r) {
      const auto &row = rows[r];
      const float ry = table_card.y + table_header_h + thead_h +
                       static_cast<float>(r) * row_h;
      const bool selected = selected_trajectory_event_ == r + 1;
      if (selected)
        surface.fill_rect({table_card.x + dp(8), ry + dp(1),
                           table_card.width - dp(16), row_h - dp(2)},
                          {254, 243, 214, 160}, dp(6));
      else if (hovered(Rect{table_card.x + dp(8), ry + dp(1),
                            table_card.width - dp(16), row_h - dp(2)}))
        surface.fill_rect({table_card.x + dp(8), ry + dp(1),
                           table_card.width - dp(16), row_h - dp(2)},
                          main_background, dp(6));
      const auto badge = trajectory_badge(row.type);
      label(surface, std::to_string(r + 1),
            {table_card.x + dp(12), ry + dp(8), col_num - dp(6), dp(14)}, 10.5,
            muted, 450, 1, white::TextAlign::left, true);
      label(surface, row.time,
            {table_card.x + dp(12) + col_num, ry + dp(8), col_time - dp(4),
             dp(14)},
            10, muted, 450, 1, white::TextAlign::left, true);
      const Rect badge_rect{table_card.x + dp(12) + col_num + col_time,
                            ry + dp(6), col_type - dp(8), dp(16)};
      surface.fill_rect(badge_rect, badge_background(row.type), dp(4));
      label(surface, badge.label, badge_rect, 9, badge.color, 700, 1,
            white::TextAlign::center, true);
      label(surface, row.role,
            {badge_rect.x + badge_rect.width + dp(6), ry + dp(8),
             col_role - dp(6), dp(14)},
            11, secondary, 450);
      label(surface, utf8_prefix(row.content, 44),
            {badge_rect.x + badge_rect.width + col_role, ry + dp(8),
             col_content - dp(10), dp(14)},
            10.5, ink, 450, 1, white::TextAlign::left, true);
      label(surface, row.duration,
            {table_card.x + table_card.width - dp(12) - col_right, ry + dp(8),
             dp(52), dp(14)},
            10, muted, 450, 1, white::TextAlign::right, true);
      label(surface, row.tokens,
            {table_card.x + table_card.width - dp(12) - dp(44), ry + dp(8),
             dp(40), dp(14)},
            10, muted, 450, 1, white::TextAlign::right, true);
      hits_.push_back({{table_card.x + dp(8), ry + dp(1),
                        table_card.width - dp(16), row_h - dp(2)},
                       WorkbenchActionKind::redraw, {},
                       "traj:" + std::to_string(r + 1)});
    }

    // Pagination footer.
    const float page_y = table_card.y + table_card.height - pagination_h;
    surface.line(table_card.x + dp(10), page_y,
                 table_card.x + table_card.width - dp(10), page_y,
                 hairline_soft);
    label(surface,
          "显示 1-" + std::to_string(std::min<std::size_t>(rows.size(), 12)) +
              " 条，共 " + std::to_string(rows.size()) + " 条",
          {table_card.x + dp(14), page_y + dp(9), dp(200), dp(14)}, 10, muted,
          450);
    const Rect page_pill{table_card.x + table_card.width - dp(120),
                         page_y + dp(5), dp(24), dp(22)};
    surface.fill_rect(page_pill, selected_fill, dp(5));
    surface.stroke_rect(page_pill, selected_border, 1, dp(5));
    label(surface, "1", page_pill, 10, gold_dark, 700, 1,
          white::TextAlign::center);
    label(surface, "20 条/页",
          {table_card.x + table_card.width - dp(90), page_y + dp(9), dp(70),
           dp(14)},
          10, tertiary, 500);

    // Event detail inspector.
    const Rect insp{insp_x, table_y, insp_w,
                    table_card.height};
    surface.fill_rect(insp, panel, dp(14));
    surface.stroke_rect(insp, hairline, 1, dp(14));
    surface.fill_circle(insp.x + dp(18), insp.y + dp(17), dp(4), ink);
    label(surface,
          "Request #" + std::to_string(selected_trajectory_event_),
          {insp.x + dp(28), insp.y + dp(10), dp(120), dp(16)}, 12, ink, 700);
    const Rect turn_pill{insp.x + insp.width - dp(88), insp.y + dp(9), dp(62),
                          dp(18)};
    surface.fill_rect(turn_pill, hover_quiet, dp(9));
    label(surface, "Turn 1", turn_pill, 9.5, tertiary, 550, 1,
          white::TextAlign::center, true);
    surface.line(insp.x + dp(12), insp.y + dp(32), insp.x + insp.width - dp(12),
                 insp.y + dp(32), hairline_soft);

    const char *detail_tabs[] = {"Summary", "Options", "Usage", "Timing"};
    float dtx = insp.x + dp(14);
    for (std::size_t t = 0; t < std::size(detail_tabs); ++t) {
      const float text_w = visual_units(detail_tabs[t]) * dp(9.4F);
      const Rect tab{dtx, insp.y + dp(38), text_w + dp(8), dp(24)};
      if (static_cast<int>(t) == trajectory_detail_tab_)
        label(surface, detail_tabs[t], tab, 11, amber, 700, 1);
      else if (hovered(tab))
        label(surface, detail_tabs[t], tab, 11, ink, 500, 1);
      else
        label(surface, detail_tabs[t], tab, 11, muted, 500, 1);
      hits_.push_back({tab, WorkbenchActionKind::redraw, {},
                       "trajtab:" + std::to_string(t)});
      dtx += tab.width + dp(12);
    }
    surface.line(insp.x + dp(12), insp.y + dp(66), insp.x + insp.width - dp(12),
                 insp.y + dp(66), hairline_soft);

    float detail_y = insp.y + dp(76);
    const auto detail_row = [&](std::string_view caption,
                                std::string_view value, Color value_color) {
      label(surface, caption,
            {insp.x + dp(14), detail_y, dp(110), dp(15)}, 10.5, muted, 450);
      label(surface, value,
            {insp.x + dp(110), detail_y, insp.width - dp(128), dp(15)}, 10.5,
            value_color, 550, 1, white::TextAlign::right, true);
      detail_y += dp(26);
    };
    if (trajectory_detail_tab_ == 0) {
      const Rect status_pill{insp.x + insp.width - dp(96), detail_y - dp(3),
                             dp(82), dp(20)};
      surface.fill_rect(status_pill, {220, 252, 231, 255}, dp(5));
      label(surface, "Completed", status_pill, 9.5,
            {22, 101, 52, 255}, 650, 1, white::TextAlign::center);
      detail_y += dp(26);
      detail_row("Provider", "deepseek-official", ink);
      detail_row("Model", frame.model.empty() ? "deepseek-v4-flash"
                                              : utf8_prefix(frame.model, 22),
                 ink);
      detail_row("Tool calls", "1", ink);
      label(surface, "Result",
            {insp.x + dp(14), detail_y, dp(110), dp(15)}, 10.5, muted, 450);
      label(surface, "Assistant Message",
            {insp.x + dp(110), detail_y, insp.width - dp(140), dp(15)}, 10.5,
            amber, 600);
      draw_icon(surface, "chevron", insp.x + insp.width - dp(20),
                detail_y + dp(7), muted);
      detail_y += dp(34);
    } else {
      label(surface, "暂无详细数据",
            {insp.x + dp(14), detail_y, insp.width - dp(28), dp(15)}, 10.5,
            faint, 450);
      detail_y += dp(34);
    }
    surface.line(insp.x + dp(12), detail_y, insp.x + insp.width - dp(12),
                 detail_y, hairline_soft);
    detail_y += dp(8);
    const char *accordions[] = {"Options", "Usage", "Timing"};
    for (const auto *entry : accordions) {
      if (hovered(Rect{insp.x + dp(10), detail_y, insp.width - dp(20), dp(24)}))
        surface.fill_rect({insp.x + dp(10), detail_y, insp.width - dp(20),
                           dp(24)},
                          main_background, dp(6));
      label(surface, entry, {insp.x + dp(14), detail_y + dp(4), dp(120), dp(15)},
            10.5, tertiary, 500);
      draw_icon(surface, "chevron", insp.x + insp.width - dp(20),
                detail_y + dp(12), faint);
      detail_y += dp(28);
    }

    // Wheel support bounds.
    trajectory_max_scroll_ =
        std::max(0.0F, (table_y + table_card.height + pad +
                        trajectory_scroll_) -
                           (body.y + body.height));
    trajectory_scroll_ = std::clamp(trajectory_scroll_, 0.0F,
                                    trajectory_max_scroll_);
    surface.pop_clip();
  }

  // ------------------------------------------------------------
  // FLOATING COMPOSER (bottom of the main column)
  // ------------------------------------------------------------
  const auto &composer = last_layout_.composer;
  const float card_w = std::min(dp(760.0F), composer.width);
  const float card_x = composer.x + (composer.width - card_w) / 2;
  const Rect card{card_x, composer.y, card_w, composer.height};
  surface.fill_rect({card.x + 2, card.y + 3, card.width, card.height},
                    {28, 25, 23, 18}, dp(16));
  surface.fill_rect(card, panel, dp(16));
  surface.stroke_rect(card,
                      frame.message_focused ? Color{245, 158, 11, 255}
                                            : hairline,
                      1, dp(16));

  const Rect message_editor{card.x + dp(14), card.y + dp(12),
                            card.width - dp(28), dp(38)};
  message_editor_bounds_ = message_editor;
  message_editor_text_ = frame.message_input;
  if (frame.message_input.empty()) {
    label(surface, "提出后续修改要求", message_editor, 13.5F,
          faint, 400, 1);
    if (frame.message_focused && frame.caret_visible)
      surface.line(message_editor.x, message_editor.y + 2, message_editor.x,
                   message_editor.y + dp(18), amber, 1.4F);
  } else {
    draw_editor_text(surface, frame.message_input, message_editor,
                     frame.editor_cursor, frame.selection_start,
                     frame.selection_end, frame.message_focused,
                     frame.caret_visible, 13.0F, 2);
  }

  if (!frame.attachments.empty()) {
    const Rect chip{card.x + dp(12), card.y - dp(26), dp(120), dp(22)};
    surface.fill_rect(chip, gold_pill_bg, dp(6));
    surface.stroke_rect(chip, gold_pill_border, 1, dp(6));
    draw_icon(surface, "file", chip.x + dp(12), chip.y + dp(11), gold_dark);
    label(surface, frame.attachments.front().name,
          {chip.x + dp(24), chip.y + dp(4), chip.width - dp(44), dp(14)}, 10,
          gold_dark, 500);
    const Rect remove{chip.x + chip.width - dp(20), chip.y + dp(2), dp(18),
                      dp(18)};
    if (hovered(chip))
      surface.fill_rect(remove, hover_fill, dp(4));
    label(surface, "×", remove, 11, danger, 600, 1, white::TextAlign::center);
    hits_.push_back({chip, WorkbenchActionKind::remove_attachment});
  }

  // Composer bottom toolbar from the linked Figma Make: attachment, access
  // level, context capacity, model, reasoning strength, and send.
  const float row_y = card.y + card.height - dp(36);
  const auto model_name =
      frame.model.empty() ? std::string("faster-whisper-large-v3-turbo")
                          : frame.model;
  const bool compact_toolbar = card.width < dp(560);

  const Rect attach{card.x + dp(12), row_y, dp(28), dp(28)};
  if (hovered(attach)) surface.fill_rect(attach, hover_quiet, dp(8));
  draw_icon(surface, "plus", attach.x + dp(14), attach.y + dp(14), muted);
  add_hit(attach, WorkbenchActionKind::attach_files);

  const Rect access{attach.x + attach.width + dp(8), row_y,
                    compact_toolbar ? dp(82) : dp(102), dp(28)};
  if (hovered(access) || composer_menu_ == 1)
    surface.fill_rect(access, selected_fill, dp(8));
  draw_icon(surface, "shield-alert", access.x + dp(12), access.y + dp(14),
            amber_deep);
  label(surface, selected_access_,
        {access.x + dp(compact_toolbar ? 22 : 24), access.y + dp(6),
         access.width - dp(compact_toolbar ? 32 : 38), dp(16)},
        compact_toolbar ? 10.5F : 11.5F, amber_deep, 550);
  draw_icon(surface, "down", access.x + access.width - dp(10),
            access.y + dp(14), amber_deep);
  hits_.push_back(
      {access, WorkbenchActionKind::redraw, {}, "menu:access"});

  const Rect send{card.x + card.width - dp(40), row_y, dp(28), dp(28)};
  const float reasoning_width = compact_toolbar ? dp(42) : dp(70);
  const Rect reasoning{send.x - reasoning_width - dp(8), row_y,
                       reasoning_width, dp(28)};
  const float model_width = compact_toolbar
                                ? std::clamp(card.width * 0.24F, dp(86), dp(120))
                                : std::clamp(card.width * 0.32F, dp(142), dp(226));
  const Rect model{reasoning.x - model_width - dp(6), row_y, model_width,
                   dp(28)};
  const Rect context{model.x - dp(34), row_y, dp(28), dp(28)};
  const bool show_context_control = card.width >= dp(460);

  if (show_context_control) {
    if (hovered(context) || composer_menu_ == 4)
      surface.fill_rect(context, hover_quiet, dp(8));
    draw_icon(surface, "circle-dashed", context.x + dp(14),
              context.y + dp(14), muted);
    hits_.push_back(
        {context, WorkbenchActionKind::redraw, {}, "menu:context"});
  }

  if (hovered(model) || composer_menu_ == 2)
    surface.fill_rect(model, hover_quiet, dp(8));
  label(surface, utf8_prefix(model_name, 30),
        {model.x + dp(7), model.y + dp(6), model.width - dp(24), dp(16)},
        11.5F, secondary, 550, 1, white::TextAlign::left, true);
  draw_icon(surface, "down", model.x + model.width - dp(10),
            model.y + dp(14), muted);
  hits_.push_back({model, WorkbenchActionKind::redraw, {}, "menu:model"});

  if (hovered(reasoning) || composer_menu_ == 3)
    surface.fill_rect(reasoning, hover_quiet, dp(8));
  draw_icon(surface, "brain", reasoning.x + dp(11), reasoning.y + dp(14),
            muted);
  if (!compact_toolbar)
    label(surface, selected_reasoning_,
          {reasoning.x + dp(22), reasoning.y + dp(6), dp(30), dp(16)}, 11.5F,
          secondary, 550);
  draw_icon(surface, "down", reasoning.x + reasoning.width - dp(9),
            reasoning.y + dp(14), muted);
  hits_.push_back(
      {reasoning, WorkbenchActionKind::redraw, {}, "menu:reasoning"});

  surface.fill_rect(send, hovered(send) ? Color{75, 85, 99, 255}
                                        : Color{107, 114, 128, 255},
                    dp(10));
  draw_icon(surface, frame.turn_active ? "stop" : "arrow-up",
            send.x + dp(14), send.y + dp(14), panel);
  add_hit(send, frame.turn_active ? WorkbenchActionKind::cancel_turn
                                  : WorkbenchActionKind::submit_input);

  // Context capacity popover.
  if (composer_menu_ == 4) {
    const Rect popup{card.x + card.width - dp(332), row_y - dp(326), dp(320),
                     dp(318)};
    composer_menu_bounds_ = {
        std::min(popup.x, context.x), popup.y,
        std::max(popup.x + popup.width, context.x + context.width) -
            std::min(popup.x, context.x),
        row_y + dp(28) - popup.y};
    surface.fill_rect({popup.x + 2, popup.y + 4, popup.width, popup.height},
                      {28, 25, 23, 30}, dp(14));
    surface.fill_rect(popup, panel, dp(14));
    surface.stroke_rect(popup, hairline, 1, dp(14));
    label(surface, "上下文容量",
          {popup.x + dp(16), popup.y + dp(14), dp(120), dp(18)}, 13, ink,
          700);
    label(surface, "31.7万/100万 (31.7%)",
          {popup.x + dp(142), popup.y + dp(14), popup.width - dp(158),
           dp(18)},
          10.5F, muted, 500, 1, white::TextAlign::right, true);
    const Rect capacity{popup.x + dp(16), popup.y + dp(46),
                        popup.width - dp(32), dp(8)};
    surface.fill_rect(capacity, hairline_soft, dp(4));
    surface.fill_rect({capacity.x, capacity.y, capacity.width * 0.317F,
                       capacity.height},
                      Color{245, 166, 35, 255}, dp(4));
    const std::array<std::pair<std::string_view, std::string_view>, 6> rows{{
        {"消息", "94.4%"}, {"系统工具", "4.5%"}, {"系统提示词", "0.4%"},
        {"技能", "0.3%"}, {"MCP 工具", "0.3%"}, {"其他", "0%"}}};
    float py = popup.y + dp(68);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      surface.fill_circle(popup.x + dp(20), py + dp(7), dp(3),
                          Color{59, static_cast<std::uint8_t>(130 + i * 14),
                                246, 255});
      label(surface, rows[i].first,
            {popup.x + dp(32), py, dp(130), dp(16)}, 11.5F, tertiary, 450);
      label(surface, rows[i].second,
            {popup.x + dp(180), py, popup.width - dp(196), dp(16)}, 11.5F,
            ink_soft, 550, 1, white::TextAlign::right, true);
      py += dp(24);
    }
    surface.line(popup.x + dp(16), py, popup.x + popup.width - dp(16), py,
                 hairline_soft);
    label(surface, "平均缓存命中率",
          {popup.x + dp(16), py + dp(12), dp(150), dp(16)}, 11.5F,
          tertiary, 450);
    label(surface, "98.6%",
          {popup.x + dp(200), py + dp(12), popup.width - dp(216), dp(16)},
          11.5F, ink, 650, 1, white::TextAlign::right, true);
    surface.line(popup.x + dp(16), py + dp(38),
                 popup.x + popup.width - dp(16), py + dp(38), hairline_soft);
    label(surface, "剩余额度",
          {popup.x + dp(16), py + dp(50), dp(120), dp(16)}, 12, ink, 650);
    label(surface, "5 小时 15% · 06:28     每周 43% · 8月26日",
          {popup.x + dp(16), py + dp(72), popup.width - dp(32), dp(16)}, 10,
          muted, 500, 1, white::TextAlign::left, true);
  } else if (composer_menu_ != 0) {
    std::vector<std::string> items;
    std::string_view heading;
    std::string prefix;
    std::string selected;
    Rect anchor = access;
    float menu_width = dp(144);
    if (composer_menu_ == 1) {
      heading = "访问级别";
      prefix = "access:";
      selected = selected_access_;
      items = {"完全访问", "受信路径", "按需确认"};
    } else if (composer_menu_ == 2) {
      heading = "选择模型";
      prefix = "model:";
      selected = model_name;
      anchor = model;
      menu_width = dp(224);
      items = {"faster-whisper-large-v3-turbo", "whisper-large-v3",
               "deepseek-v4-flash", "gpt-4o-transcribe"};
    } else {
      heading = "推理强度";
      prefix = "reasoning:";
      selected = selected_reasoning_;
      anchor = reasoning;
      menu_width = dp(128);
      items = {"最高", "标准", "低"};
    }
    const float menu_h =
        dp(26) + static_cast<float>(items.size()) * dp(30) + dp(6);
    const float menu_x =
        composer_menu_ == 1 ? anchor.x
                            : anchor.x + anchor.width - menu_width;
    const Rect menu{menu_x, row_y - menu_h - dp(8), menu_width, menu_h};
    composer_menu_bounds_ = {menu.x, menu.y, menu.width,
                             row_y + dp(28) - menu.y};
    surface.fill_rect({menu.x + 2, menu.y + 4, menu.width, menu.height},
                      {28, 25, 23, 30}, dp(12));
    surface.fill_rect(menu, panel, dp(12));
    surface.stroke_rect(menu, hairline, 1, dp(12));
    label(surface, heading,
          {menu.x + dp(12), menu.y + dp(7), menu.width - dp(24), dp(14)}, 10,
          faint, 600);
    surface.line(menu.x + dp(10), menu.y + dp(26),
                 menu.x + menu.width - dp(10), menu.y + dp(26),
                 hairline_soft);
    float my = menu.y + dp(30);
    for (const auto &item : items) {
      const Rect row{menu.x + dp(6), my, menu.width - dp(12), dp(28)};
      const bool is_selected = item == selected;
      if (is_selected || hovered(row))
        surface.fill_rect(row, selected_fill, dp(8));
      label(surface, utf8_prefix(item, 32),
            {row.x + dp(10), row.y + dp(6), row.width - dp(30), dp(16)}, 11,
            is_selected ? amber_deep : secondary,
            is_selected ? 700 : 500, 1, white::TextAlign::left,
            composer_menu_ == 2);
      if (is_selected)
        draw_icon(surface, "check", menu.x + menu.width - dp(16),
                  my + dp(14), amber);
      hits_.push_back({row, WorkbenchActionKind::redraw, {}, prefix + item});
      my += dp(30);
    }
  }

  // ============================================================
  // COLUMN 3: RIGHT CODE INSPECTOR
  // ============================================================
  if (last_layout_.viewer_visible) {
    const auto &viewer = last_layout_.viewer;
    surface.fill_rect(viewer, panel);
    surface.line(viewer.x, viewer.y, viewer.x, viewer.y + viewer.height,
                 hairline);

    // Tabs: 代码审阅 / 文件预览 (Figma: 46px design header, items-center).
    const auto &viewer_header = last_layout_.viewer_header;
    float vtab_x = viewer_header.x + dp(16);
    const auto viewer_tab = [&](std::string_view text, bool active,
                                const std::string &id) {
      const float text_w = visual_units(text) * dp(12.2F);
      const Rect tab{vtab_x, viewer_header.y, text_w + dp(16),
                     viewer_header.height};
      const Rect text_rect{tab.x, tab.y + (tab.height - dp(20)) / 2,
                           tab.width, dp(20)};
      if (active) {
        label(surface, text, text_rect, 13.5, amber_deep, 600, 1,
              white::TextAlign::center);
        surface.fill_rect({tab.x + dp(6), tab.y + tab.height - dp(2),
                           tab.width - dp(12), dp(2)},
                          amber, 1);
      } else if (hovered(tab)) {
        label(surface, text, text_rect, 13.5, ink, 500, 1,
              white::TextAlign::center);
      } else {
        label(surface, text, text_rect, 13.5, muted, 500, 1,
              white::TextAlign::center);
      }
      hits_.push_back({tab, WorkbenchActionKind::viewer_tab, {}, id});
      vtab_x += tab.width + dp(16);
    };
    viewer_tab("代码审阅", viewer_tab_ == "code", "code");
    viewer_tab("文件预览", viewer_tab_ == "preview", "preview");
    surface.line(viewer.x, viewer_header.y + viewer_header.height,
                 viewer.x + viewer.width,
                 viewer_header.y + viewer_header.height, hairline);

    // Sub header: file selector + diff stat.
    const float sub_y = viewer_header.y + viewer_header.height;
    surface.fill_rect({viewer.x, sub_y, viewer.width, dp(38)}, main_background);
    surface.line(viewer.x, sub_y + dp(38), viewer.x + viewer.width,
                 sub_y + dp(38), hairline);
    const bool has_real_doc =
        !selected_document_.empty() && selected_document_ != "Welcome" &&
        std::ranges::find(open_documents_, selected_document_) !=
            open_documents_.end();
    const std::string shown_file =
        has_real_doc ? selected_document_.filename().string() : viewer_demo_file_;
    const float file_text_w = visual_units(shown_file) * dp(8.4F);
    const Rect file_btn{viewer.x + dp(10), sub_y + dp(5),
                        file_text_w + dp(64), dp(28)};
    if (hovered(file_btn))
      surface.fill_rect(file_btn, hover_fill, dp(7));
    const auto file_ext = std::filesystem::path(shown_file).extension().string();
    const Color file_dot = file_ext == ".py"     ? Color{220, 242, 254, 255}
                           : file_ext == ".yaml" ? Color{254, 243, 214, 255}
                                                 : Color{243, 232, 255, 255};
    surface.fill_circle(file_btn.x + dp(13), file_btn.y + dp(14), dp(8),
                        file_dot);
    draw_icon(surface, "file", file_btn.x + dp(13), file_btn.y + dp(14),
              file_ext == ".py" ? info_blue
                                : (file_ext == ".yaml" ? amber_deep
                                                       : Color{124, 58, 237, 255}));
    label(surface, shown_file,
          {file_btn.x + dp(26), file_btn.y + dp(6), file_text_w + dp(6),
           dp(16)},
          11.5, ink, 600, 1, white::TextAlign::left, true);
    draw_icon(surface, "down", file_btn.x + file_btn.width - dp(12),
              file_btn.y + dp(14), faint);
    hits_.push_back({file_btn, WorkbenchActionKind::redraw, {}, "file-menu"});

    label(surface, "+42",
          {viewer.x + viewer.width - dp(108), sub_y + dp(10), dp(34), dp(16)},
          11, success_deep, 700, 1, white::TextAlign::right, true);
    label(surface, "-0",
          {viewer.x + viewer.width - dp(70), sub_y + dp(10), dp(26), dp(16)},
          11, danger, 700, 1, white::TextAlign::right, true);
    icon_button({viewer.x + viewer.width - dp(40), sub_y + dp(5), dp(28),
                 dp(28)},
                "more", WorkbenchActionKind::redraw, "");

    // File selector dropdown.
    if (viewer_file_menu_) {
      std::vector<std::string> entries{"transcribe.py", "config.yaml",
                                       "output.srt"};
      for (const auto &doc : open_documents_) {
        auto name = doc.filename().string();
        if (std::ranges::find(entries, name) == entries.end())
          entries.push_back(std::move(name));
      }
      const float menu_w = dp(170);
      const float menu_h = dp(6) +
                           static_cast<float>(entries.size()) * dp(30) +
                           dp(6);
      const Rect menu{file_btn.x, sub_y + dp(36), menu_w, menu_h};
      // Include the anchor button so clicking the toggle keeps toggling.
      viewer_menu_bounds_ = {menu.x, menu.y, menu.width,
                             file_btn.y + file_btn.height - menu.y};
      surface.fill_rect({menu.x + 2, menu.y + 3, menu.width, menu.height},
                        {28, 25, 23, 30}, dp(10));
      surface.fill_rect(menu, panel, dp(10));
      surface.stroke_rect(menu, hairline, 1, dp(10));
      float my = menu.y + dp(6);
      for (const auto &entry : entries) {
        const Rect row{menu.x + dp(6), my, menu.width - dp(12), dp(28)};
        const bool selected = entry == shown_file;
        if (selected || hovered(row))
          surface.fill_rect(row, selected_fill, dp(8));
        label(surface, entry,
              {row.x + dp(10), row.y + dp(6), row.width - dp(30), dp(16)}, 11,
              selected ? amber_deep : secondary, selected ? 700 : 500, 1,
              white::TextAlign::left, true);
        if (selected)
          draw_icon(surface, "check", menu.x + menu.width - dp(16), my + dp(14),
                    amber);
        hits_.push_back({row, WorkbenchActionKind::redraw, {},
                         "file:" + entry});
        my += dp(30);
      }
    }

    // Body: code review or file preview.
    const auto &doc_area = last_layout_.document;
    if (doc_area.width > 0) {
      if (viewer_tab_ == "code") {
        surface.fill_rect(doc_area, panel);
        std::span<const std::string_view> demo_lines;
        if (shown_file == "config.yaml")
          demo_lines = demo_config_yaml;
        else if (shown_file == "output.srt")
          demo_lines = demo_output_srt;
        else if (shown_file == "transcribe.py")
          demo_lines = demo_transcribe_py;

        const float line_h = dp(20);
        std::size_t total_lines = has_real_doc && demo_lines.empty()
                                      ? document_lines_.size()
                                      : demo_lines.size();
        if (has_real_doc && demo_lines.empty() && total_lines == 0)
          total_lines = 1;
        document_max_scroll_ =
            std::max(0.0F, static_cast<float>(total_lines) * line_h -
                               doc_area.height);
        document_scroll_ =
            std::clamp(document_scroll_, 0.0F, document_max_scroll_);

        surface.push_clip(doc_area);
        float ly = doc_area.y + dp(8) - document_scroll_;
        std::size_t index = 0;
        const auto draw_code_line = [&](std::string_view line) {
          if (ly + line_h > doc_area.y && ly < doc_area.y + doc_area.height) {
            label(surface, std::to_string(index + 1),
                  {doc_area.x + dp(6), ly + dp(2), dp(26), dp(16)}, 10,
                  Color{214, 211, 209, 255}, 450, 1, white::TextAlign::right,
                  true);
            const auto spans = code_spans(line, 11.5F);
            surface.rich_paragraph(
                spans, {doc_area.x + dp(42), ly + dp(1),
                        doc_area.width - dp(50), line_h},
                1.35F);
          }
          ly += line_h;
          ++index;
        };
        if (has_real_doc && demo_lines.empty()) {
          for (const auto &line : document_lines_)
            draw_code_line(line);
        } else {
          for (const auto &line : demo_lines)
            draw_code_line(line);
        }
        surface.pop_clip();
      } else {
        // File preview: subtitle cards.
        surface.fill_rect(doc_area, panel);
        float py = doc_area.y + dp(10);
        label(surface, "字幕预览 (output.srt)",
              {doc_area.x + dp(12), py, doc_area.width - dp(150), dp(18)}, 12,
              ink, 700);
        const Rect copy_sub{doc_area.x + doc_area.width - dp(96), py - dp(3),
                            dp(86), dp(26)};
        if (hovered(copy_sub))
          surface.fill_rect(copy_sub, hover_fill, dp(7));
        surface.stroke_rect(copy_sub, hairline, 1, dp(7));
        draw_icon(surface, "copy", copy_sub.x + dp(12), copy_sub.y + dp(13),
                  tertiary);
        label(surface, "复制字幕",
              {copy_sub.x + dp(22), copy_sub.y + dp(6), copy_sub.width - dp(28),
               dp(14)},
              10.5, tertiary, 500);
        hits_.push_back({copy_sub, WorkbenchActionKind::copy_text, {},
                         subtitle_export_text()});
        py += dp(32);

        const Rect sub_search{doc_area.x + dp(12), py, doc_area.width - dp(24),
                              dp(28)};
        surface.fill_rect(sub_search, sidebar_background, dp(8));
        surface.stroke_rect(sub_search, hairline, 1, dp(8));
        draw_icon(surface, "search", sub_search.x + dp(12),
                  sub_search.y + dp(14), faint);
        label(surface, "在字幕中搜索...",
              {sub_search.x + dp(26), sub_search.y + dp(6),
               sub_search.width - dp(38), dp(16)},
              10.5, faint, 400);
        py += dp(38);

        for (const auto &sub : subtitle_items) {
          if (py > doc_area.y + doc_area.height - dp(60))
            break;
          const Rect sub_card{doc_area.x + dp(12), py, doc_area.width - dp(24),
                              dp(56)};
          surface.fill_rect(sub_card, main_background, dp(12));
          surface.stroke_rect(sub_card, hairline, 1, dp(12));
          label(surface, "#" + std::to_string(sub.id),
                {sub_card.x + dp(12), sub_card.y + dp(8), dp(30), dp(14)}, 10,
                amber, 650, 1, white::TextAlign::left, true);
          label(surface, std::string(sub.start) + " ➔ " + std::string(sub.end),
                {sub_card.x + sub_card.width - dp(150), sub_card.y + dp(8),
                 dp(138), dp(14)},
                10, amber, 500, 1, white::TextAlign::right, true);
          label(surface, sub.text,
                {sub_card.x + dp(12), sub_card.y + dp(28),
                 sub_card.width - dp(24), dp(20)},
                11.5, ink, 450);
          py += dp(64);
        }
      }
    }

    // Inspector footer status bar.
    const float status_y = viewer.y + viewer.height - 32;
    surface.line(viewer.x, status_y, viewer.x + viewer.width, status_y,
                 hairline);
    surface.fill_rect({viewer.x, status_y + 1, viewer.width, 31},
                      main_background);
    draw_icon(surface, "check", viewer.x + dp(18), status_y + dp(16),
              success_deep);
    label(surface, "审阅完成",
          {viewer.x + dp(28), status_y + dp(8), dp(80), dp(16)}, 10.5,
              success_text, 550);
    label(surface, "Python  |  UTF-8  |  2 个问题",
          {viewer.x + viewer.width - dp(180), status_y + dp(8), dp(160),
           dp(16)},
          10.5, muted, 450, 1, white::TextAlign::right);
    draw_icon(surface, "down", viewer.x + viewer.width - dp(14),
              status_y + dp(16), muted);
  }

  // ============================================================
  // SETTINGS MODAL (1120x720, three columns)
  // ============================================================
  settings_modal_bounds_ = {};
  settings_editor_bounds_ = {};
  settings_editor_text_.clear();
  settings_editor_field_.clear();

  if (settings_open_) {
    hits_.clear();
    hover_regions_.clear();
    surface.fill_rect({0, 0, width, height}, {28, 25, 23, 90});

    const float modal_w = std::min(dp(1120.0F), width * 0.92F);
    const float modal_h = std::min(dp(720.0F), height * 0.86F);
    const Rect modal{(width - modal_w) / 2, (height - modal_h) / 2, modal_w,
                     modal_h};
    settings_modal_bounds_ = modal;
    surface.fill_rect({modal.x + dp(3), modal.y + dp(5), modal.width,
                       modal.height},
                      {28, 25, 23, 40}, dp(16));
    surface.fill_rect(modal, panel, dp(16));
    surface.stroke_rect(modal, hairline, 1, dp(16));

    // Header (56px).
    label(surface, "设置",
          {modal.x + dp(28), modal.y + dp(15), dp(120), dp(28)}, 19, ink,
          750);
    const Rect search_box{modal.x + (modal.width - dp(360)) / 2.0F,
                          modal.y + dp(11), dp(360), dp(34)};
    surface.fill_rect(search_box, sidebar_background, dp(10));
    surface.stroke_rect(search_box, hairline, 1, dp(10));
    draw_icon(surface, "search", search_box.x + dp(14), search_box.y + dp(17),
              faint);
    label(surface, "搜索设置项",
          {search_box.x + dp(30), search_box.y + dp(9),
           search_box.width - dp(42), dp(16)},
          11.5, faint, 400);
    icon_button({modal.x + modal.width - dp(44), modal.y + dp(12), dp(32),
                 dp(32)},
                "window-close", WorkbenchActionKind::close_settings);
    surface.line(modal.x, modal.y + dp(56), modal.x + modal.width,
                 modal.y + dp(56),
                 Color{240, 238, 232, 255});

    // Left navigation (220px).
    const Rect nav{modal.x, modal.y + dp(56), dp(220),
                   modal_h - dp(56 + 56)};
    surface.fill_rect(nav, {251, 251, 249, 255});
    struct NavItem {
      std::string id;
      std::string icon;
      std::string text;
    };
    const NavItem nav_items[] = {
        {"general", "settings", "通用"},
        {"models", "agent", "智能体与模型"},
        {"security", "lock", "权限与安全"},
        {"workspace", "folder", "工作区"},
        {"notifications", "bell", "通知"},
        {"appearance", "palette", "外观"},
        {"shortcuts", "keyboard", "快捷键"},
        {"account", "user", "账户"}};
    float nav_y = nav.y + dp(12);
    for (const auto &item : nav_items) {
      const Rect row{nav.x + dp(12), nav_y, nav.width - dp(24), dp(38)};
      const bool selected = settings_tab_ == item.id;
      if (selected || hovered(row))
        surface.fill_rect(row, selected ? selected_fill : Color{243, 242, 236, 255},
                          dp(10));
      draw_icon(surface, item.icon, row.x + dp(16), row.y + dp(19),
                selected ? amber : muted);
      label(surface, item.text,
            {row.x + dp(34), row.y + dp(10), row.width - dp(44), dp(18)}, 12,
            selected ? gold_dark : tertiary, selected ? 650 : 500);
      hits_.push_back({row, WorkbenchActionKind::settings_tab, {}, item.id});
      nav_y += dp(42);
    }

    // Center form area.
    const float form_x = modal.x + dp(220 + 28);
    const float form_w = modal_w - dp(220 + 230 + 56);
    const Rect overview{modal.x + modal_w - dp(230), modal.y + dp(56),
                        dp(230), modal_h - dp(56 + 56)};
    surface.fill_rect(overview, {253, 251, 247, 255});
    surface.line(overview.x, overview.y, overview.x,
                 overview.y + overview.height, Color{240, 238, 232, 255});
    float ov_y = overview.y + dp(18);

    const auto row_label = [&](std::string_view text, float y) {
      label(surface, text, {form_x, y + dp(8), form_w * 0.5F, dp(20)}, 12, ink,
            550);
    };
    const auto row_divider = [&](float y) {
      surface.line(form_x, y + dp(44), form_x + form_w, y + dp(44),
                   hairline_soft);
    };

    const auto draw_segmented =
        [&](float y, std::span<const std::pair<std::string_view, std::string_view>> opts,
            std::string_view active_val, std::string_view key_name) {
          const float seg_w = form_w * 0.48F;
          const Rect seg{form_x + form_w - seg_w, y, seg_w, dp(32)};
          surface.fill_rect(seg, hover_quiet, dp(10));
          surface.stroke_rect(seg, hairline, 1, dp(10));
          const float opt_w = seg.width / static_cast<float>(opts.size());
          for (std::size_t i = 0; i < opts.size(); ++i) {
            const Rect o{seg.x + static_cast<float>(i) * opt_w, seg.y, opt_w,
                         seg.height};
            const bool active = opts[i].second == active_val;
            if (active) {
              surface.fill_rect({o.x + dp(2), o.y + dp(2), o.width - dp(4),
                                 o.height - dp(4)},
                                selected_fill, dp(8));
            } else if (hovered(o)) {
              surface.fill_rect({o.x + dp(2), o.y + dp(2), o.width - dp(4),
                                 o.height - dp(4)},
                                hover_fill, dp(8));
            }
            label(surface, opts[i].first, o, 11, active ? gold_dark : muted,
                  active ? 650 : 500, 1, white::TextAlign::center);
            hits_.push_back({o, WorkbenchActionKind::set_setting, {},
                             std::string(key_name) + "=" +
                                 std::string(opts[i].second)});
          }
        };
    const auto draw_dropdown =
        [&](float y, std::string_view text, std::string_view key_name,
            std::string_view next_val) {
          const float drop_w = form_w * 0.48F;
          const Rect drop{form_x + form_w - drop_w, y, drop_w, dp(32)};
          surface.fill_rect(drop, sidebar_background, dp(10));
          surface.stroke_rect(drop, hairline, 1, dp(10));
          label(surface, text,
                {drop.x + dp(14), drop.y + dp(8), drop.width - dp(36), dp(16)},
                11.5, ink, 500, 1, white::TextAlign::left, true);
          draw_icon(surface, "down", drop.x + drop.width - dp(14),
                    drop.y + dp(16), muted);
          hits_.push_back({drop, WorkbenchActionKind::set_setting, {},
                           std::string(key_name) + "=" + std::string(next_val)});
        };
    const auto draw_toggle = [&](float y, bool val, std::string_view key) {
      const Rect sw{form_x + form_w - dp(52), y + dp(3), dp(44), dp(24)};
      surface.fill_rect(sw, val ? gold_accent : Color{231, 229, 228, 255},
                        dp(12));
      surface.fill_circle(sw.x + (val ? dp(30) : dp(14)), sw.y + dp(12),
                          dp(9), panel);
      hits_.push_back({sw, WorkbenchActionKind::set_setting, {},
                       std::string(key) + "=toggle"});
    };

    float fy = modal.y + dp(56 + 24);

    const auto overview_line = [&](std::string_view caption,
                                   std::string_view value, Color color) {
      label(surface, caption,
            {overview.x + dp(18), ov_y, overview.width - dp(36), dp(14)}, 10.5,
            faint, 450);
      label(surface, value,
            {overview.x + dp(18), ov_y + dp(15), overview.width - dp(36),
             dp(16)},
            11.5, color, 650, 1, white::TextAlign::left, true);
      ov_y += dp(42);
    };

    if (settings_tab_ == "general") {
      row_label("应用语言", fy);
      draw_dropdown(fy, language_label(frame.settings.language), "language",
                    frame.settings.language == "zh-CN" ||
                            frame.settings.language == "简体中文"
                        ? "English"
                        : "zh-CN");
      row_divider(fy);
      fy += dp(58);
      row_label("启动时打开", fy);
      constexpr std::pair<std::string_view, std::string_view> start_opts[] = {
          {"首页", "home"}, {"上次打开的会话", "last_session"}};
      draw_segmented(fy, start_opts, frame.settings.open_on_startup,
                     "open_on_startup");
      row_divider(fy);
      fy += dp(58);
      row_label("自动保存", fy);
      draw_dropdown(fy, frame.settings.auto_save_interval, "auto_save_interval",
                    frame.settings.auto_save_interval == "5 分钟" ? "10 分钟"
                                                                 : "5 分钟");
      row_divider(fy);
      fy += dp(58);
      row_label("更新通道", fy);
      constexpr std::pair<std::string_view, std::string_view> chan_opts[] = {
          {"稳定版", "stable"}, {"测试版", "beta"}};
      draw_segmented(fy, chan_opts, frame.settings.update_channel,
                     "update_channel");
    } else if (settings_tab_ == "models") {
      row_label("默认智能体", fy);
      draw_dropdown(fy, frame.settings.default_agent, "default_agent",
                    frame.settings.default_agent == "代码助手"
                        ? "翻译助手"
                        : "代码助手");
      row_divider(fy);
      fy += dp(58);
      row_label("模型提供方", fy);
      constexpr std::pair<std::string_view, std::string_view> prov_opts[] = {
          {"Tokmon 官方", "official"}, {"自定义", "custom"}};
      draw_segmented(fy, prov_opts, frame.settings.provider_mode,
                     "provider_mode");
      row_divider(fy);
      fy += dp(58);
      row_label("provider_id", fy);
      {
        const float field_w = form_w * 0.48F;
        const Rect input{form_x + form_w - field_w, fy, field_w, dp(32)};
        surface.fill_rect(input, panel, dp(8));
        surface.stroke_rect(
            input,
            frame.active_settings_field == "provider_id" ? amber : hairline, 1,
            dp(8));
        settings_editor_bounds_ = input;
        settings_editor_text_ = frame.settings.provider_id;
        settings_editor_field_ = "provider_id";
        label(surface, utf8_prefix(frame.settings.provider_id, 30),
              {input.x + dp(12), input.y + dp(8), input.width - dp(24), dp(16)},
              11, secondary, 450, 1, white::TextAlign::left, true);
        if (!frame.settings_field_focused ||
            frame.active_settings_field != "provider_id") {
          hits_.push_back({input, WorkbenchActionKind::focus_settings_field, {},
                           "provider_id"});
        }
      }
      row_divider(fy);
      fy += dp(58);
      row_label("主模型", fy);
      draw_dropdown(fy, utf8_prefix(frame.settings.model, 34), "model",
                    frame.settings.model == "faster-whisper-large-v3-turbo"
                        ? "whisper-large-v3"
                        : "faster-whisper-large-v3-turbo");
      row_divider(fy);
      fy += dp(58);
      row_label("推理强度", fy);
      constexpr std::pair<std::string_view, std::string_view> eff_opts[] = {
          {"低", "low"}, {"标准", "standard"}, {"高", "high"}};
      draw_segmented(fy, eff_opts, frame.settings.reasoning_effort,
                     "reasoning_effort");
    } else if (settings_tab_ == "security") {
      row_label("文件访问", fy);
      draw_dropdown(fy, file_access_label(frame.settings.file_access),
                    "file_access",
                    frame.settings.file_access == "trusted" ||
                            frame.settings.file_access == "受信路径"
                        ? "all"
                        : "trusted");
      row_divider(fy);
      fy += dp(58);
      row_label("命令审批", fy);
      constexpr std::pair<std::string_view, std::string_view> app_opts[] = {
          {"自动执行", "auto"}, {"按需确认", "on_demand"}, {"禁止执行", "deny"}};
      draw_segmented(fy, app_opts, frame.settings.command_approval,
                     "command_approval");
      row_divider(fy);
      fy += dp(58);
      row_label("网络访问", fy);
      draw_toggle(fy, frame.settings.network_access, "network_access");
      row_divider(fy);
      fy += dp(58);
      row_label("高风险二次确认", fy);
      draw_toggle(fy, frame.settings.high_risk_confirm, "high_risk_confirm");
    } else if (settings_tab_ == "workspace") {
      row_label("默认工作区", fy);
      {
        const float field_w = form_w * 0.44F;
        const Rect input{form_x + form_w - field_w, fy, field_w, dp(32)};
        surface.fill_rect(input, panel, dp(8));
        surface.stroke_rect(input, hairline, 1, dp(8));
        label(surface, utf8_prefix(frame.settings.default_workspace, 36),
              {input.x + dp(12), input.y + dp(8), input.width - dp(34), dp(16)},
              10.5, secondary, 450, 1, white::TextAlign::left, true);
        draw_icon(surface, "folder", input.x + input.width - dp(16),
                  input.y + dp(16), muted);
        hits_.push_back({input, WorkbenchActionKind::set_setting, {},
                         "default_workspace=" + frame.settings.default_workspace});
      }
      row_divider(fy);
      fy += dp(58);
      row_label("索引模式", fy);
      draw_dropdown(fy, index_mode_label(frame.settings.index_mode),
                    "index_mode",
                    frame.settings.index_mode == "standard" ||
                            frame.settings.index_mode == "标准"
                        ? "deep"
                        : "standard");
      row_divider(fy);
      fy += dp(58);
      row_label("自动同步", fy);
      draw_toggle(fy, frame.settings.auto_sync, "auto_sync");
      row_divider(fy);
      fy += dp(58);
      row_label("Git 集成", fy);
      draw_toggle(fy, frame.settings.git_integration, "git_integration");
    } else if (settings_tab_ == "notifications") {
      row_label("启用通知", fy);
      draw_toggle(fy, frame.settings.enable_notifications,
                  "enable_notifications");
      row_divider(fy);
      fy += dp(58);
      row_label("桌面通知", fy);
      draw_toggle(fy, frame.settings.desktop_notifications,
                  "desktop_notifications");
      row_divider(fy);
      fy += dp(58);
      row_label("消息提醒", fy);
      draw_toggle(fy, frame.settings.message_alerts, "message_alerts");
      row_divider(fy);
      fy += dp(58);
      row_label("免打扰", fy);
      draw_dropdown(fy, frame.settings.dnd_hours, "dnd_hours",
                    frame.settings.dnd_hours == "22:00 - 08:00" ? "23:00 - 07:00"
                                                               : "22:00 - 08:00");
    } else if (settings_tab_ == "appearance") {
      row_label("主题模式", fy);
      constexpr std::pair<std::string_view, std::string_view> theme_opts[] = {
          {"浅色", "light"}, {"深色", "dark"}};
      draw_segmented(fy, theme_opts, frame.settings.theme, "theme");
      row_divider(fy);
      fy += dp(58);
      row_label("强调色", fy);
      {
        const Color colors[] = {gold_accent,
                                {244, 63, 94, 255},
                                {168, 85, 247, 255},
                                {59, 130, 246, 255},
                                {34, 197, 94, 255},
                                {107, 114, 128, 255}};
        const std::string names[] = {"gold", "rose", "purple",
                                     "blue", "green", "gray"};
        float dot_x = form_x + form_w - dp(6 * 32 - 8);
        for (std::size_t c = 0; c < 6; ++c) {
          const bool active = frame.settings.accent_color == names[c] ||
                              (frame.settings.accent_color.empty() && c == 0);
          const Rect dot{dot_x, fy + dp(3), dp(24), dp(24)};
          if (active)
            surface.stroke_rect({dot.x - 3, dot.y - 3, dot.width + 6,
                                 dot.height + 6},
                                gold_accent, 1.6F, dp(15));
          surface.fill_circle(dot.x + dp(12), dot.y + dp(12), dp(11),
                              colors[c]);
          hits_.push_back({dot, WorkbenchActionKind::set_setting, {},
                           "accent_color=" + names[c]});
          dot_x += dp(32);
        }
      }
      row_divider(fy);
      fy += dp(58);
      row_label("界面密度", fy);
      constexpr std::pair<std::string_view, std::string_view> den_opts[] = {
          {"紧凑", "compact"}, {"舒适", "comfortable"}, {"宽松", "loose"}};
      draw_segmented(fy, den_opts, frame.settings.ui_density, "ui_density");
      row_divider(fy);
      fy += dp(58);
      row_label("字体大小", fy);
      {
        const float track_w = form_w * 0.44F - dp(56);
        const Rect track{form_x + form_w - track_w - dp(56), fy + dp(13),
                         track_w, dp(5)};
        surface.fill_rect(track, hairline, dp(2.5F));
        const auto fraction =
            static_cast<float>(frame.settings.font_size_percent - 80) / 40.0F;
        surface.fill_rect({track.x, track.y, track.width * fraction,
                           track.height},
                          gold_accent, dp(2.5F));
        surface.fill_circle(track.x + track.width * fraction, track.y + dp(2),
                            dp(7), gold_accent);
        label(surface, std::to_string(frame.settings.font_size_percent) + "%",
              {track.x + track.width + dp(10), fy + dp(7), dp(46), dp(16)}, 11,
              muted, 500, 1, white::TextAlign::right, true);
        hits_.push_back({Rect{track.x - dp(4), fy, track.width + dp(8),
                              dp(32)},
                         WorkbenchActionKind::redraw, {}, "font-slider"});
      }
    } else if (settings_tab_ == "shortcuts") {
      const auto shortcut_row = [&](std::string_view title,
                                    std::span<const std::string_view> keys,
                                    float y) {
        const Rect row{form_x, y, form_w, dp(44)};
        surface.fill_rect(row, main_background, dp(10));
        surface.stroke_rect(row, hairline, 1, dp(10));
        label(surface, title,
              {row.x + dp(14), row.y + dp(13), dp(160), dp(18)}, 12, ink, 550);
        float k_x = row.x + row.width - dp(14);
        for (const auto &k : std::views::reverse(keys)) {
          const float k_w = utf8_length(k) > 2 ? dp(44) : dp(32);
          k_x -= k_w;
          const Rect k_box{k_x, row.y + dp(9), k_w, dp(26)};
          surface.fill_rect(k_box, panel, dp(6));
          surface.stroke_rect(k_box, hairline, 1, dp(6));
          label(surface, k, k_box, 10, secondary, 550, 1,
                white::TextAlign::center);
          if (&k != &keys.front()) {
            k_x -= dp(14);
            label(surface, "+", {k_x, row.y + dp(13), dp(12), dp(18)}, 10,
                  faint, 450, 1, white::TextAlign::center);
          }
        }
      };
      constexpr std::string_view k1[] = {"Ctrl", "N"};
      shortcut_row("新建会话", k1, fy);
      fy += dp(54);
      constexpr std::string_view k2[] = {"Ctrl", ","};
      shortcut_row("打开设置", k2, fy);
      fy += dp(54);
      constexpr std::string_view k3[] = {"Enter"};
      shortcut_row("发送消息", k3, fy);
      fy += dp(54);
      constexpr std::string_view k4[] = {"Ctrl", "Shift", "P"};
      shortcut_row("命令面板", k4, fy);
    } else if (settings_tab_ == "account") {
      surface.fill_circle(form_x + form_w / 2, fy + dp(28), dp(28),
                          Color{254, 240, 138, 255});
      draw_icon(surface, "user", form_x + form_w / 2, fy + dp(28), gold_dark);
      fy += dp(66);
      const auto account_row = [&](std::string_view caption,
                                   std::string_view value, Color color) {
        const Rect row{form_x, fy, form_w, dp(40)};
        surface.fill_rect(row, main_background, dp(10));
        surface.stroke_rect(row, hairline, 1, dp(10));
        label(surface, caption,
              {row.x + dp(14), row.y + dp(11), dp(120), dp(18)}, 12, ink, 550);
        label(surface, value,
              {row.x + dp(140), row.y + dp(11), row.width - dp(180), dp(18)},
              11.5, color, 500, 1, white::TextAlign::right, true);
        draw_icon(surface, "chevron", row.x + row.width - dp(18),
                  row.y + dp(20), faint);
        fy += dp(48);
      };
      account_row("昵称", frame.settings.account_name, secondary);
      account_row("登录邮箱", frame.settings.account_email, secondary);
      account_row("当前方案", frame.settings.account_plan, amber_deep);
      row_label("云同步", fy + dp(4));
      draw_toggle(fy, frame.settings.cloud_sync, "cloud_sync");
    }

    // Right overview panel.
    const auto overview_title = [&](std::string_view text) {
      label(surface, text,
            {overview.x + dp(18), ov_y, overview.width - dp(36), dp(18)}, 12,
            ink, 700);
      surface.line(overview.x + dp(18), ov_y + dp(24),
                   overview.x + overview.width - dp(18), ov_y + dp(24),
                   Color{243, 241, 233, 255});
      ov_y += dp(34);
    };
    if (settings_tab_ == "general") {
      overview_title("通用概览");
      overview_line("语言", language_label(frame.settings.language), ink);
      overview_line("启动", startup_label(frame.settings.open_on_startup), ink);
      overview_line("更新通道",
                    update_channel_label(frame.settings.update_channel), ink);
    } else if (settings_tab_ == "models") {
      overview_title("模型概览");
      overview_line("默认智能体", frame.settings.default_agent, ink);
      overview_line("模型提供方",
                    provider_mode_label(frame.settings.provider_mode), ink);
      overview_line("主模型", utf8_prefix(frame.settings.model, 26), ink);
    } else if (settings_tab_ == "security") {
      overview_title("安全概览");
      overview_line("文件访问", file_access_label(frame.settings.file_access),
                    ink);
      overview_line("命令审批",
                    approval_label(frame.settings.command_approval), ink);
      overview_line("确认状态",
                    frame.settings.high_risk_confirm ? "已开启二次确认" : "未开启",
                    frame.settings.high_risk_confirm ? success_deep : muted);
    } else if (settings_tab_ == "workspace") {
      overview_title("工作区概览");
      overview_line("路径", utf8_prefix(frame.settings.default_workspace, 26),
                    ink);
      overview_line("索引模式", index_mode_label(frame.settings.index_mode),
                    ink);
      overview_line("自动同步", frame.settings.auto_sync ? "已开启" : "已关闭",
                    ink);
    } else if (settings_tab_ == "notifications") {
      overview_title("通知概览");
      overview_line("通知状态",
                    frame.settings.enable_notifications ? "已启用" : "已禁用",
                    frame.settings.enable_notifications ? success_deep : muted);
      overview_line("桌面通知",
                    frame.settings.desktop_notifications ? "已启用" : "已禁用",
                    ink);
      overview_line("免打扰时间", frame.settings.dnd_hours, ink);
    } else if (settings_tab_ == "appearance") {
      overview_title("外观概览");
      overview_line("主题", theme_label(frame.settings.theme), ink);
      overview_line("强调色", "浅金色", ink);
      overview_line("密度", density_label(frame.settings.ui_density), ink);
    } else if (settings_tab_ == "shortcuts") {
      overview_title("快捷键概览");
      overview_line("预设方案", "Tokmon 默认", ink);
      overview_line("已修改", "0 项", ink);
      overview_line("冲突状态", "无冲突", success_deep);
    } else if (settings_tab_ == "account") {
      overview_title("账户概览");
      overview_line("昵称", frame.settings.account_name, ink);
      overview_line("方案", frame.settings.account_plan, amber_deep);
      overview_line("云同步", frame.settings.cloud_sync ? "● 已开启" : "○ 已关闭",
                    frame.settings.cloud_sync ? success_deep : muted);
    }
    const Rect restore_btn{overview.x + dp(18),
                           overview.y + overview.height - dp(48),
                           overview.width - dp(36), dp(30)};
    if (hovered(restore_btn))
      surface.fill_rect(restore_btn, hover_fill, dp(8));
    surface.stroke_rect(restore_btn, hairline, 1, dp(8));
    label(surface, "恢复默认设置", restore_btn, 10.5, tertiary, 500, 1,
          white::TextAlign::center);
    hits_.push_back({restore_btn, WorkbenchActionKind::set_setting, {},
                     "reset_defaults"});

    // Footer (56px).
    const float footer_y = modal.y + modal_h - dp(56);
    surface.line(modal.x, footer_y, modal.x + modal.width, footer_y,
                 Color{240, 238, 232, 255});
    const Rect cancel_btn{modal.x + modal.width - dp(240), footer_y + dp(11),
                          dp(96), dp(34)};
    if (hovered(cancel_btn))
      surface.fill_rect(cancel_btn, hover_fill, dp(10));
    else
      surface.fill_rect(cancel_btn, hover_quiet, dp(10));
    label(surface, "取消", cancel_btn, 12, tertiary, 550, 1,
          white::TextAlign::center);
    add_hit(cancel_btn, WorkbenchActionKind::close_settings);
    const Rect save_btn{modal.x + modal.width - dp(132), footer_y + dp(11),
                        dp(104), dp(34)};
    surface.fill_rect(save_btn, hovered(save_btn) ? gold_pill_hover
                                                  : selected_fill,
                      dp(10));
    surface.stroke_rect(save_btn, selected_border, 1, dp(10));
    label(surface, "保存更改", save_btn, 12, gold_dark, 650, 1,
          white::TextAlign::center);
    add_hit(save_btn, WorkbenchActionKind::save_settings);
  }

  // ============================================================
  // FLOATING WINDOW CONTROLS (top-right overlay)
  // ============================================================
  {
    const Rect minimize{width - dp(110), dp(8), dp(30), dp(30)};
    const Rect maximize{width - dp(74), dp(8), dp(30), dp(30)};
    const Rect close{width - dp(38), dp(8), dp(30), dp(30)};
    if (hovered(minimize))
      surface.fill_rect(minimize, hover_fill, dp(6));
    if (hovered(maximize))
      surface.fill_rect(maximize, hover_fill, dp(6));
    const bool close_hover = hovered(close);
    if (close_hover)
      surface.fill_rect(close, danger, dp(6));
    draw_icon(surface, "window-minimize", minimize.x + dp(15),
              minimize.y + dp(15), muted);
    draw_icon(surface,
              frame.window_maximized ? "window-restore" : "window-maximize",
              maximize.x + dp(15), maximize.y + dp(15), muted);
    draw_icon(surface, "window-close", close.x + dp(15), close.y + dp(15),
              close_hover ? Color{255, 255, 255, 255} : muted);
    add_hit(minimize, WorkbenchActionKind::window_minimize);
    add_hit(maximize, WorkbenchActionKind::window_toggle_maximize);
    add_hit(close, WorkbenchActionKind::window_close);
  }

  // Resizers between columns.
  if (last_layout_.sidebar_splitter.width > 0) {
    const auto &splitter = last_layout_.sidebar_splitter;
    if (hovered(splitter) || resizing_sidebar_)
      surface.fill_rect({splitter.x + 2, splitter.y, 2, splitter.height},
                        gold_accent);
  }
  if (last_layout_.viewer_splitter.width > 0) {
    const auto &splitter = last_layout_.viewer_splitter;
    if (hovered(splitter) || resizing_viewer_)
      surface.fill_rect({splitter.x + 2, splitter.y, 2, splitter.height},
                        gold_accent);
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
    if (trajectory_open_ &&
        (trajectory_search_bounds_.contains(event.x, event.y) ||
         (event.x >= 800 && event.x <= 1000 && event.y >= 95 && event.y <= 135))) {
      request_redraw(trajectory_search_bounds_);
      selecting_input_ = true;
      selecting_editor_ = "trajectory";
      return {WorkbenchActionKind::focus_trajectory_search, {}, 0,
              editor_offset_at(event.x, event.y, trajectory_search_bounds_,
                               trajectory_search_text_),
              false};
    }
    if (last_layout_.sidebar_visible &&
        filter_editor_bounds_.contains(event.x, event.y)) {
      request_redraw(filter_editor_bounds_);
      selecting_input_ = true;
      selecting_filter_ = true;
      selecting_editor_ = "filter";
      return {WorkbenchActionKind::focus_filter, {}, 0,
              editor_offset_at(event.x, event.y, filter_editor_bounds_,
                               filter_editor_text_),
              false};
    }
    if (rename_editor_bounds_.width > 0 &&
        rename_editor_bounds_.contains(event.x, event.y)) {
      request_redraw(rename_editor_bounds_);
      selecting_input_ = true;
      selecting_filter_ = false;
      selecting_editor_ = "rename";
      return {WorkbenchActionKind::focus_rename, {}, 0,
              editor_offset_at(event.x, event.y, rename_editor_bounds_,
                               rename_editor_text_),
              false};
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
              editor_offset_at(event.x, event.y, message_editor_bounds_,
                               message_editor_text_),
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
      const auto max_sidebar = std::min(
          dp(420.0F),
          std::max(dp(180.0F), last_layout_.bounds.width - dp(400.0F)));
      const auto next_width =
          std::clamp(event.x, dp(180.0F), max_sidebar);
      const bool changed = next_width != sidebar_width_ || sidebar_collapsed_ ||
                           !sidebar_manually_sized_;
      sidebar_width_ = next_width;
      sidebar_collapsed_ = false;
      sidebar_manually_sized_ = true;
      if (changed) request_redraw();
      return {changed ? WorkbenchActionKind::redraw : WorkbenchActionKind::none,
              {},
              0, 0, false, true};
    }
    if (resizing_viewer_) {
      const auto available =
          last_layout_.bounds.width -
          (last_layout_.sidebar_visible ? last_layout_.sidebar.width : 0.0F);
      const auto max_viewer = std::min(
          dp(720.0F), std::max(dp(320.0F), available - dp(400.0F)));
      const auto next_width = std::clamp(
          last_layout_.bounds.width - event.x, dp(320.0F), max_viewer);
      const bool changed = next_width != viewer_width_ || viewer_collapsed_ ||
                           !viewer_manually_sized_;
      viewer_width_ = next_width;
      viewer_collapsed_ = false;
      viewer_manually_sized_ = true;
      if (changed) request_redraw();
      return {changed ? WorkbenchActionKind::redraw : WorkbenchActionKind::none,
              {},
              0, 0, false, true};
    }
    if (selecting_input_) {
      const auto &bounds =
          selecting_editor_ == "settings"  ? settings_editor_bounds_
          : selecting_editor_ == "filter"  ? filter_editor_bounds_
          : selecting_editor_ == "rename"  ? rename_editor_bounds_
          : selecting_editor_ == "trajectory" ? trajectory_search_bounds_
                                              : message_editor_bounds_;
      const auto &text =
          selecting_editor_ == "settings"  ? settings_editor_text_
          : selecting_editor_ == "filter"  ? filter_editor_text_
          : selecting_editor_ == "rename"  ? rename_editor_text_
          : selecting_editor_ == "trajectory" ? trajectory_search_text_
                                              : message_editor_text_;
      const auto cursor = editor_offset_at(event.x, event.y, bounds, text);
      if (cursor == editor_cursor_) return {};
      editor_cursor_ = cursor;
      request_redraw(bounds);
      return {WorkbenchActionKind::set_editor_cursor, {}, 0, cursor, true,
              false};
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
    if (settings_open_) return {};
    if (trajectory_open_ && last_layout_.timeline.contains(event.x, event.y)) {
      const auto next = std::clamp(trajectory_scroll_ + event.delta_y, 0.0F,
                                   trajectory_max_scroll_);
      if (next == trajectory_scroll_) return {};
      trajectory_scroll_ = next;
      request_redraw(last_layout_.timeline);
      return {WorkbenchActionKind::redraw};
    }
    if (!trajectory_open_ && last_layout_.timeline.contains(event.x, event.y)) {
      const auto next =
          std::clamp(timeline_scroll_ + event.delta_y, 0.0F,
                     timeline_max_scroll_);
      if (next == timeline_scroll_) return {};
      timeline_scroll_ = next;
      follow_tail_ = (timeline_scroll_ >= timeline_max_scroll_ - 2);
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

  request_redraw();
  // Any click outside an open floating menu closes it, even when the click
  // lands on another control.
  if (composer_menu_ != 0 &&
      !composer_menu_bounds_.contains(event.x, event.y))
    composer_menu_ = 0;
  if (viewer_file_menu_ && !viewer_menu_bounds_.contains(event.x, event.y))
    viewer_file_menu_ = false;
  if (resizing_sidebar_ || resizing_viewer_) {
    resizing_sidebar_ = false;
    resizing_viewer_ = false;
    const bool over_splitter =
        last_layout_.sidebar_splitter.contains(event.x, event.y) ||
        last_layout_.viewer_splitter.contains(event.x, event.y);
    pointer_cursor_active_ = over_splitter;
    return {WorkbenchActionKind::none, {}, 0, 0, false, over_splitter};
  }
  selecting_input_ = false;
  selecting_editor_.clear();

  for (const auto &target : std::views::reverse(hits_)) {
    if (!target.bounds.contains(event.x, event.y))
      continue;

    // Presentation-only toggles handled inside the view.
    if (target.action == WorkbenchActionKind::redraw) {
      if (target.value.starts_with("tree:")) {
        const auto key = target.value.substr(5);
        if (tree_collapsed_.contains(key))
          tree_collapsed_.erase(key);
        else
          tree_collapsed_.insert(key);
        return {WorkbenchActionKind::redraw};
      }
      if (target.value == "workflow") {
        workflow_expanded_ = !workflow_expanded_;
        return {WorkbenchActionKind::redraw};
      }
      if (target.value == "menu:access") {
        composer_menu_ = composer_menu_ == 1 ? 0 : 1;
        return {WorkbenchActionKind::redraw};
      }
      if (target.value == "menu:model") {
        composer_menu_ = composer_menu_ == 2 ? 0 : 2;
        return {WorkbenchActionKind::redraw};
      }
      if (target.value == "menu:reasoning") {
        composer_menu_ = composer_menu_ == 3 ? 0 : 3;
        return {WorkbenchActionKind::redraw};
      }
      if (target.value == "menu:context") {
        composer_menu_ = composer_menu_ == 4 ? 0 : 4;
        return {WorkbenchActionKind::redraw};
      }
      if (target.value.starts_with("access:")) {
        selected_access_ = target.value.substr(7);
        composer_menu_ = 0;
        return {WorkbenchActionKind::redraw};
      }
      if (target.value.starts_with("reasoning:")) {
        selected_reasoning_ = target.value.substr(10);
        composer_menu_ = 0;
        return {WorkbenchActionKind::redraw};
      }
      if (target.value.starts_with("model:")) {
        composer_menu_ = 0;
        return {WorkbenchActionKind::set_setting,
                "model=" + target.value.substr(6)};
      }
      if (target.value == "file-menu") {
        viewer_file_menu_ = !viewer_file_menu_;
        return {WorkbenchActionKind::redraw};
      }
      if (target.value.starts_with("file:")) {
        const auto name = target.value.substr(5);
        viewer_file_menu_ = false;
        viewer_demo_file_ = name;
        const auto existing = std::ranges::find(
            open_documents_, name,
            [](const std::filesystem::path &doc) {
              return doc.filename().string();
            });
        if (existing != open_documents_.end())
          open_document(*existing);
        return {WorkbenchActionKind::redraw};
      }
      if (target.value.starts_with("traj:")) {
        selected_trajectory_event_ =
            static_cast<std::size_t>(std::stoul(target.value.substr(5)));
        return {WorkbenchActionKind::redraw};
      }
      if (target.value.starts_with("trajtab:")) {
        trajectory_detail_tab_ =
            std::stoi(target.value.substr(8));
        return {WorkbenchActionKind::redraw};
      }
      if (target.value == "font-slider") {
        const auto fraction =
            std::clamp((event.x - target.bounds.x) / target.bounds.width,
                       0.0F, 1.0F);
        const auto percent = 80 + static_cast<int>(std::round(fraction * 8.0F)) * 5;
        return {WorkbenchActionKind::set_setting,
                "font_size_percent=" + std::to_string(std::min(120, percent))};
      }
    }
    if (target.action == WorkbenchActionKind::toggle_left_panel) {
      sidebar_collapsed_ = !sidebar_collapsed_;
      return {WorkbenchActionKind::redraw};
    }
    if (target.action == WorkbenchActionKind::toggle_right_panel) {
      viewer_collapsed_ = !viewer_collapsed_;
      return {WorkbenchActionKind::redraw};
    }
    if (target.action == WorkbenchActionKind::open_settings) {
      settings_open_ = true;
      return {WorkbenchActionKind::open_settings};
    }
    if (target.action == WorkbenchActionKind::close_settings) {
      settings_open_ = false;
      return {WorkbenchActionKind::close_settings};
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
    if (target.action == WorkbenchActionKind::set_trajectory_filter) {
      trajectory_filter_ = trajectory_filter_ == "all"    ? "turns"
                           : trajectory_filter_ == "turns" ? "calls"
                                                           : "all";
      return {WorkbenchActionKind::set_trajectory_filter, trajectory_filter_};
    }
    if (target.action == WorkbenchActionKind::scroll_to_tail) {
      timeline_scroll_ = timeline_max_scroll_;
      follow_tail_ = true;
      return {WorkbenchActionKind::redraw};
    }
    return {target.action, target.value, target.index};
  }

  // Outside click closes any open floating menu.
  if (composer_menu_ != 0 &&
      !composer_menu_bounds_.contains(event.x, event.y)) {
    composer_menu_ = 0;
    return {WorkbenchActionKind::redraw};
  }
  if (viewer_file_menu_ &&
      !viewer_menu_bounds_.contains(event.x, event.y)) {
    viewer_file_menu_ = false;
    return {WorkbenchActionKind::redraw};
  }
  return {};
}

} // namespace tokmon::desktop
