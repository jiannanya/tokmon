#include <tokmon/app.hpp>

#include <iostream>

int main(int argc, char** argv) {
  try {
    auto workspace = std::filesystem::current_path();
    std::string config_dir_name = ".tokmon";
    bool headless_smoke = false;
    for (int index = 1; index < argc; ++index) {
      const std::string value = argv[index];
      if (value == "--workspace" && index + 1 < argc) {
        workspace = argv[++index];
      } else if (value == "--config-dir-name" && index + 1 < argc) {
        config_dir_name = argv[++index];
      } else if (value == "--headless-smoke") {
        headless_smoke = true;
      }
    }
    auto config =
        tokmon::desktop::load_app_config(workspace, config_dir_name);
    tokmon::desktop::App app(std::move(config));
    return headless_smoke ? app.smoke() : app.run();
  } catch (const tokmon::Error& error) {
    std::cerr << error.code() << ": " << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
