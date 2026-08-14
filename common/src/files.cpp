#include <tokmon/common/files.hpp>
#include <tokmon/common/types.hpp>

#include <fstream>
#include <cstdlib>
#include <sstream>

namespace tokmon {

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw Error("file.open", "failed to open file: " + path.string());
  }
  std::ostringstream content;
  content << input.rdbuf();
  if (!input.good() && !input.eof()) {
    throw Error("file.read", "failed to read file: " + path.string());
  }
  return content.str();
}

void write_text_file_atomic(const std::filesystem::path& path,
                            std::string_view content) {
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  auto temporary = path;
  temporary += ".tmp-" + make_uuid();

  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw Error("file.open", "failed to create file: " + temporary.string());
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) {
      throw Error("file.write", "failed to write file: " + temporary.string());
    }
  }

  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
  }
  if (error) {
    std::filesystem::remove(temporary);
    throw Error("file.commit", "failed to atomically replace file: " +
                                   path.string());
  }
}

std::filesystem::path canonical_within(const std::filesystem::path& root,
                                       const std::filesystem::path& candidate,
                                       bool require_existing) {
  const auto normalized_root = std::filesystem::weakly_canonical(root);
  const auto joined = candidate.is_absolute() ? candidate : root / candidate;
  const auto normalized = require_existing
                              ? std::filesystem::canonical(joined)
                              : std::filesystem::weakly_canonical(joined);

  auto root_it = normalized_root.begin();
  auto value_it = normalized.begin();
  for (; root_it != normalized_root.end() && value_it != normalized.end();
       ++root_it, ++value_it) {
#ifdef _WIN32
    auto a = root_it->wstring();
    auto b = value_it->wstring();
    std::transform(a.begin(), a.end(), a.begin(), ::towlower);
    std::transform(b.begin(), b.end(), b.begin(), ::towlower);
    if (a != b) {
      throw Error("path.outside_workspace",
                  "path escapes workspace: " + normalized.string());
    }
#else
    if (*root_it != *value_it) {
      throw Error("path.outside_workspace",
                  "path escapes workspace: " + normalized.string());
    }
#endif
  }
  if (root_it != normalized_root.end()) {
    throw Error("path.outside_workspace",
                "path escapes workspace: " + normalized.string());
  }
  return normalized;
}

std::optional<std::string> environment_variable(std::string_view name) {
  if (name.empty() || name.find('=') != std::string_view::npos) return std::nullopt;
  const std::string key(name);
#ifdef _WIN32
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, key.c_str()) != 0 || !value) return std::nullopt;
  std::string result(value, length > 0 ? length - 1 : 0);
  std::free(value);
  return result;
#else
  if (const auto* value = std::getenv(key.c_str())) return std::string(value);
  return std::nullopt;
#endif
}

} // namespace tokmon
