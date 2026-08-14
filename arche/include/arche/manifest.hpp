#pragma once

#include <arche/types.hpp>

#include <filesystem>

namespace arche {

[[nodiscard]] PluginDescriptor parse_plugin_manifest(const Json& document);
[[nodiscard]] PluginDescriptor load_plugin_manifest(
    const std::filesystem::path& path);
void validate_plugin_descriptor(const PluginDescriptor& descriptor);

} // namespace arche

