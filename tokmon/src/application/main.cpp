#include <tokmon/app.hpp>

#include <algorithm>
#include <iostream>
#include <optional>

int main(int argc, char** argv) {
  try {
    auto workspace = std::filesystem::current_path();
    std::string config_dir_name = ".tokmon";
    bool headless_smoke = false;
    bool screenshot_demo = false;
    std::string screenshot_state;
    std::optional<float> ui_scale;
    std::optional<int> window_width;
    std::optional<int> window_height;
    std::filesystem::path screenshot;
    for (int index = 1; index < argc; ++index) {
      const std::string value = argv[index];
      if (value == "--workspace" && index + 1 < argc) {
        workspace = argv[++index];
      } else if (value == "--config-dir-name" && index + 1 < argc) {
        config_dir_name = argv[++index];
      } else if (value == "--headless-smoke") {
        headless_smoke = true;
      } else if (value == "--screenshot" && index + 1 < argc) {
        screenshot = argv[++index];
      } else if (value == "--screenshot-demo") {
        screenshot_demo = true;
      } else if (value == "--screenshot-state" && index + 1 < argc) {
        screenshot_state = argv[++index];
      } else if (value == "--ui-scale" && index + 1 < argc) {
        ui_scale = std::stof(argv[++index]);
      } else if (value == "--window-width" && index + 1 < argc) {
        window_width = std::stoi(argv[++index]);
      } else if (value == "--window-height" && index + 1 < argc) {
        window_height = std::stoi(argv[++index]);
      }
    }
    auto config =
        tokmon::desktop::load_app_config(workspace, config_dir_name);
    if (ui_scale) config.ui_scale = std::clamp(*ui_scale, 0.75F, 2.0F);
    if (window_width) config.window_width = std::max(800, *window_width);
    if (window_height) config.window_height = std::max(600, *window_height);
    config.screenshot_state = screenshot_state;
    tokmon::desktop::App app(std::move(config));
    if (!screenshot.empty()) {
      if (screenshot_demo) {
        app.projection().append_local(
            tokmon::desktop::ItemKind::user, "You",
            "使用 faster-whisper 模型对音频文件进行转录，输出带时间戳的字幕（Segmentation 模式）。\n"
            "模型路径：C:\\Models\\faster-whisper-large-v3-turbo\n"
            "音频文件：C:\\Data\\audio.mp3\n"
            "输出字幕文件：UTF-8 编码的 .srt",
            "committed", {{"time", "2026-08-14T10:20:00.000Z"}});
        app.projection().append_local(
            tokmon::desktop::ItemKind::assistant, "Tokmon Agent",
            "已理解你的需求，我将使用 faster-whisper 进行音频转录，并输出带时间戳的字幕文件。\n"
            "我会分步骤完成任务并实时向你汇报进度。",
            "committed",
            {{"time", "2026-08-14T10:21:00.000Z"}, {"elapsed_ms", 138000}});
      }
      app.capture(screenshot);
      return 0;
    }
    return headless_smoke ? app.smoke() : app.run();
  } catch (const tokmon::Error& error) {
    std::cerr << error.code() << ": " << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
