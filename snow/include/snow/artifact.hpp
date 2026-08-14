#pragma once

#include <tokmon/common/types.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace snow {

struct BlobReference {
  std::string id;
  std::string sha256;
  std::uint64_t bytes{0};
  std::string media_type{"application/octet-stream"};
  std::string encryption{"none"};
};

void to_json(tokmon::Json& out, const BlobReference& value);
void from_json(const tokmon::Json& in, BlobReference& value);

class ArtifactStore final {
public:
  explicit ArtifactStore(std::filesystem::path root);
  [[nodiscard]] BlobReference put(
      std::span<const std::byte> content,
      std::string media_type = "application/octet-stream");
  [[nodiscard]] BlobReference put_text(
      std::string_view content, std::string media_type = "text/plain; charset=utf-8");
  [[nodiscard]] std::vector<std::byte> read(const BlobReference& reference) const;
  [[nodiscard]] bool contains(std::string_view sha256) const;
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
  [[nodiscard]] std::filesystem::path path_for(std::string_view sha256) const;
  std::filesystem::path root_;
};

class RawTraceVault final {
public:
  RawTraceVault(std::filesystem::path root, bool enabled,
                std::chrono::hours retention = std::chrono::hours(24 * 7));
  [[nodiscard]] bool enabled() const noexcept { return enabled_; }
  [[nodiscard]] BlobReference put(std::string_view kind,
                                  std::span<const std::byte> content);
  [[nodiscard]] BlobReference put_text(std::string_view kind,
                                       std::string_view content);
  [[nodiscard]] std::vector<std::byte> read(const BlobReference& reference) const;
  std::size_t purge_expired();

private:
  [[nodiscard]] std::filesystem::path data_path(std::string_view id) const;
  [[nodiscard]] std::filesystem::path metadata_path(std::string_view id) const;
  std::filesystem::path root_;
  bool enabled_{false};
  std::chrono::hours retention_{};
};

} // namespace snow
