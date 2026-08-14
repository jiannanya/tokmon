#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tokmon {

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path);
void write_text_file_atomic(const std::filesystem::path& path,
                            std::string_view content);
[[nodiscard]] std::filesystem::path canonical_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate,
    bool require_existing = false);
[[nodiscard]] std::optional<std::string> environment_variable(
    std::string_view name);

} // namespace tokmon
