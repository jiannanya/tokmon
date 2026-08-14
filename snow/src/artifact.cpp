#include <snow/artifact.hpp>

#include <tokmon/common/digest.hpp>
#include <tokmon/common/files.hpp>

#include <fstream>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>
#endif

namespace snow {
namespace {

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw tokmon::Error("snow.blob.read", "cannot read blob");
  const auto size = input.tellg();
  if (size < 0) throw tokmon::Error("snow.blob.read", "invalid blob size");
  std::vector<std::byte> result(static_cast<std::size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(result.data()), size);
  if (!input) throw tokmon::Error("snow.blob.read", "short blob read");
  return result;
}

void write_bytes_atomic(const std::filesystem::path& path,
                        std::span<const std::byte> content) {
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp-" + tokmon::make_uuid();
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw tokmon::Error("snow.blob.write", "cannot create blob");
    output.write(reinterpret_cast<const char*>(content.data()),
                 static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) throw tokmon::Error("snow.blob.write", "cannot flush blob");
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    if (std::filesystem::exists(path)) {
      std::filesystem::remove(temporary, error);
      return;
    }
    std::filesystem::remove(temporary, error);
    throw tokmon::Error("snow.blob.commit", "cannot commit blob");
  }
}

#ifdef _WIN32
std::vector<std::byte> protect(std::span<const std::byte> content) {
  DATA_BLOB source{
      static_cast<DWORD>(content.size()),
      reinterpret_cast<BYTE*>(const_cast<std::byte*>(content.data()))};
  DATA_BLOB protected_data{};
  if (!CryptProtectData(&source, L"Snow raw trace", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &protected_data)) {
    throw tokmon::Error("snow.raw-vault.encrypt", "DPAPI encryption failed");
  }
  std::vector<std::byte> result(protected_data.cbData);
  std::memcpy(result.data(), protected_data.pbData, protected_data.cbData);
  LocalFree(protected_data.pbData);
  return result;
}

std::vector<std::byte> unprotect(std::span<const std::byte> content) {
  DATA_BLOB source{
      static_cast<DWORD>(content.size()),
      reinterpret_cast<BYTE*>(const_cast<std::byte*>(content.data()))};
  DATA_BLOB clear{};
  if (!CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &clear)) {
    throw tokmon::Error("snow.raw-vault.decrypt", "DPAPI decryption failed");
  }
  std::vector<std::byte> result(clear.cbData);
  std::memcpy(result.data(), clear.pbData, clear.cbData);
  LocalFree(clear.pbData);
  return result;
}
#endif

} // namespace

void to_json(tokmon::Json& out, const BlobReference& value) {
  out = {{"id", value.id}, {"sha256", value.sha256}, {"bytes", value.bytes},
         {"media_type", value.media_type}, {"encryption", value.encryption}};
}

void from_json(const tokmon::Json& in, BlobReference& value) {
  value.id = in.at("id").get<std::string>();
  value.sha256 = in.at("sha256").get<std::string>();
  value.bytes = in.at("bytes").get<std::uint64_t>();
  value.media_type = in.value("media_type", "application/octet-stream");
  value.encryption = in.value("encryption", "none");
}

ArtifactStore::ArtifactStore(std::filesystem::path root)
    : root_(std::filesystem::absolute(std::move(root))) {
  std::filesystem::create_directories(root_ / "objects");
}

std::filesystem::path ArtifactStore::path_for(std::string_view sha256) const {
  if (sha256.size() != 64) throw tokmon::Error("snow.blob.hash", "invalid SHA-256");
  return root_ / "objects" / std::string(sha256.substr(0, 2)) /
         std::string(sha256);
}

BlobReference ArtifactStore::put(std::span<const std::byte> content,
                                 std::string media_type) {
  const auto digest = tokmon::sha256_hex(content);
  const auto path = path_for(digest);
  if (!std::filesystem::exists(path)) write_bytes_atomic(path, content);
  return {"sha256:" + digest, digest, static_cast<std::uint64_t>(content.size()),
          std::move(media_type), "none"};
}

BlobReference ArtifactStore::put_text(std::string_view content,
                                      std::string media_type) {
  return put(std::as_bytes(std::span(content.data(), content.size())),
             std::move(media_type));
}

std::vector<std::byte> ArtifactStore::read(
    const BlobReference& reference) const {
  auto content = read_bytes(path_for(reference.sha256));
  if (tokmon::sha256_hex(content) != reference.sha256)
    throw tokmon::Error("snow.blob.integrity", "artifact hash mismatch");
  return content;
}

bool ArtifactStore::contains(std::string_view sha256) const {
  return sha256.size() == 64 && std::filesystem::exists(path_for(sha256));
}

RawTraceVault::RawTraceVault(std::filesystem::path root, bool enabled,
                             std::chrono::hours retention)
    : root_(std::filesystem::absolute(std::move(root))), enabled_(enabled),
      retention_(retention) {
  if (!enabled_) return;
#ifndef _WIN32
  throw tokmon::Error("snow.raw-vault.encryption-unavailable",
                      "raw trace capture requires an encrypted vault backend");
#else
  std::filesystem::create_directories(root_);
#endif
}

std::filesystem::path RawTraceVault::data_path(std::string_view id) const {
  return root_ / (std::string(id) + ".bin");
}

std::filesystem::path RawTraceVault::metadata_path(std::string_view id) const {
  return root_ / (std::string(id) + ".json");
}

BlobReference RawTraceVault::put(std::string_view kind,
                                 std::span<const std::byte> content) {
  if (!enabled_) throw tokmon::Error("snow.raw-vault.disabled", "raw trace vault is disabled");
#ifdef _WIN32
  const auto id = tokmon::make_uuid();
  const auto digest = tokmon::sha256_hex(content);
  const auto encrypted = protect(content);
  write_bytes_atomic(data_path(id), encrypted);
  const auto expires = std::chrono::system_clock::now() + retention_;
  const auto expires_seconds = std::chrono::duration_cast<std::chrono::seconds>(
      expires.time_since_epoch()).count();
  const tokmon::Json metadata{{"schema", "org.tokmon.snow.raw-trace/v1"},
                              {"id", id}, {"kind", kind},
                              {"sha256", digest}, {"bytes", content.size()},
                              {"encryption", "dpapi-user"},
                              {"expires_unix", expires_seconds}};
  tokmon::write_text_file_atomic(metadata_path(id), metadata.dump(2));
  return {id, digest, static_cast<std::uint64_t>(content.size()),
          "application/octet-stream", "dpapi-user"};
#else
  (void)kind;
  (void)content;
  throw tokmon::Error("snow.raw-vault.encryption-unavailable",
                      "raw trace capture requires an encrypted vault backend");
#endif
}

BlobReference RawTraceVault::put_text(std::string_view kind,
                                      std::string_view content) {
  auto result = put(kind, std::as_bytes(std::span(content.data(), content.size())));
  result.media_type = "text/plain; charset=utf-8";
  return result;
}

std::vector<std::byte> RawTraceVault::read(
    const BlobReference& reference) const {
  if (!enabled_) throw tokmon::Error("snow.raw-vault.disabled", "raw trace vault is disabled");
#ifdef _WIN32
  const auto metadata = tokmon::Json::parse(
      tokmon::read_text_file(metadata_path(reference.id)));
  if (metadata.value("sha256", "") != reference.sha256)
    throw tokmon::Error("snow.raw-vault.reference", "raw trace reference mismatch");
  auto result = unprotect(read_bytes(data_path(reference.id)));
  if (tokmon::sha256_hex(result) != reference.sha256)
    throw tokmon::Error("snow.raw-vault.integrity", "raw trace hash mismatch");
  return result;
#else
  (void)reference;
  throw tokmon::Error("snow.raw-vault.encryption-unavailable",
                      "raw trace capture requires an encrypted vault backend");
#endif
}

std::size_t RawTraceVault::purge_expired() {
  if (!enabled_) return 0;
  std::size_t removed = 0;
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(root_, error)) {
    if (error || entry.path().extension() != ".json") continue;
    try {
      const auto metadata = tokmon::Json::parse(tokmon::read_text_file(entry.path()));
      if (metadata.value("expires_unix", std::int64_t{0}) > now) continue;
      const auto id = metadata.at("id").get<std::string>();
      std::filesystem::remove(data_path(id), error);
      error.clear();
      if (std::filesystem::remove(entry.path(), error)) ++removed;
      error.clear();
    } catch (...) {
      // Corrupt metadata is retained for explicit repair/diagnostics.
    }
  }
  return removed;
}

} // namespace snow
