#include <tokmon/app.hpp>

#include <iostream>

int main(int argc, char** argv) {
  try {
    auto workspace = std::filesystem::current_path();
    std::string config_dir_name = ".tokmon";
    bool headless_smoke = false;
    bool screenshot_demo = false;
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
      }
    }
    auto config =
        tokmon::desktop::load_app_config(workspace, config_dir_name);
    tokmon::desktop::App app(std::move(config));
    if (!screenshot.empty()) {
      if (screenshot_demo) {
        app.projection().append_local(
            tokmon::desktop::ItemKind::user, "You",
            "对话流不能只是卡片堆叠，要像自然文档一样清晰，并且所有操作都必须真实可用。",
            "committed", {{"time", tokmon::iso8601()}});
        app.projection().append_local(
            tokmon::desktop::ItemKind::assistant, "Snow",
            "已完成 Tokmon 对话流的重新设计，助手回复现在使用连续正文而不是消息卡片。\n\n"
            "主要能力：\n\n"
            "- **用户消息**显示为右侧圆角气泡，并提供时间、复制和再次编辑。\n"
            "- 助手区域显示真实运行耗时、状态分隔线和流式输出状态。\n"
            "- 支持标题、自然段、项目符号、**粗体**、[设计文档](docs/design.md) 和 `Shift+Enter` 行内代码。\n"
            "- 工具调用、诊断、审批与产物仍保留结构化状态卡片。\n"
            "- 长回复可以滚动，并通过底部按钮一键返回最新输出。\n\n"
            "验证结果：\n\n"
            "- 对话复制、消息编辑、Markdown 排版和轨迹耗时均有自动化测试。",
            "committed",
            {{"time", tokmon::iso8601()}, {"elapsed_ms", 2450}});
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
