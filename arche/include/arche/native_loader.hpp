#pragma once

#include <arche/plugin.hpp>

#include <filesystem>

namespace arche {

struct NativeLoadOptions {
  // v1 defaults to logical unload. Physical unload is opt-in only after a
  // plugin has passed callback/thread/TLS/allocator stress tests.
  bool physical_unload{false};
};

[[nodiscard]] PluginPtr load_native_plugin(
    const std::filesystem::path& library_path,
    NativeLoadOptions options = {});

} // namespace arche
