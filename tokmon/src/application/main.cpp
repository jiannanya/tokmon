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
    tokmon::desktop::App app(std::move(config));
    if (!screenshot.empty()) {
      if (screenshot_demo) {
        app.projection().append_local(
            tokmon::desktop::ItemKind::user, "You",
            "构建时报错，提示找不到 spdlog 和 fmt。请帮我修复 CMake 依赖配置，并确保可以通过编译和测试。",
            "committed", {{"time", tokmon::iso8601()}});
        app.projection().append_local(
            tokmon::desktop::ItemKind::assistant, "Tokmon Agent",
            "已分析仓库并执行修复，构建与测试均通过。",
            "committed",
            {{"time", tokmon::iso8601()}, {"elapsed_ms", 2450}});
        app.projection().append_local(
            tokmon::desktop::ItemKind::artifact, "CMakeLists.txt",
            "已读取 · 2.3 KB", "loaded");
        app.projection().append_local(
            tokmon::desktop::ItemKind::tool, "Terminal / shell",
            "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release\n"
            "cmake --build build --config Release -j 12\n"
            "ctest --test-dir build --output-on-failure",
            "completed");
        app.projection().append_local(
            tokmon::desktop::ItemKind::diagnostic, "Build diagnostics", "",
            "inspected");
        app.projection().append_local(
            tokmon::desktop::ItemKind::status, "修复说明",
            "1. 在 CMakeLists.txt 中补充依赖查找。\n"
            "2. 为外部依赖设置别名目标并链接到应用。\n"
            "3. 补充默认安装提示，便于 CI 与本地环境一致。\n"
            "4. 验证构建与测试，确认问题已解决。",
            "committed");
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
