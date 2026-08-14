#include <arche/package.hpp>

#include <tokmon/common/digest.hpp>
#include <tokmon/common/files.hpp>

#include <algorithm>
#include <array>
#include <set>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#else
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#endif

namespace arche {
namespace {

std::vector<std::byte> decode_base64(std::string_view input) {
  std::array<int, 256> values{};
  values.fill(-1);
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (std::size_t index = 0; index < alphabet.size(); ++index)
    values[static_cast<unsigned char>(alphabet[index])] =
        static_cast<int>(index);
  std::vector<std::byte> result;
  unsigned accumulator = 0;
  int bits = -8;
  for (const auto character : input) {
    if (character == '=') break;
    const auto value = values[static_cast<unsigned char>(character)];
    if (value < 0)
      throw tokmon::Error("arche.package.base64",
                          "invalid base64 in package signature");
    accumulator = (accumulator << 6U) | static_cast<unsigned>(value);
    bits += 6;
    if (bits >= 0) {
      result.push_back(static_cast<std::byte>((accumulator >> bits) & 0xffU));
      bits -= 8;
    }
  }
  return result;
}

std::vector<std::byte> decode_hex(std::string_view input) {
  if (input.size() != 64)
    throw tokmon::Error("arche.package.digest", "invalid SHA-256 digest");
  auto digit = [](char value) -> unsigned {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10U;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10U;
    throw tokmon::Error("arche.package.digest", "invalid SHA-256 digest");
  };
  std::vector<std::byte> result(32);
  for (std::size_t index = 0; index < result.size(); ++index)
    result[index] = static_cast<std::byte>(
        (digit(input[index * 2]) << 4U) | digit(input[index * 2 + 1]));
  return result;
}

bool verify_signature(std::string_view algorithm,
                      std::span<const std::byte> public_key,
                      std::span<const std::byte> digest,
                      std::span<const std::byte> signature) {
  if (algorithm != "ecdsa-p256-sha256") return false;
#ifdef _WIN32
  std::vector<std::byte> native_key;
  if (public_key.size() == 65 && public_key[0] == std::byte{0x04}) {
    native_key.resize(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(native_key.data());
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::copy(public_key.begin() + 1, public_key.end(),
              native_key.begin() + sizeof(BCRYPT_ECCKEY_BLOB));
  } else {
    native_key.assign(public_key.begin(), public_key.end());
  }
  BCRYPT_ALG_HANDLE provider = nullptr;
  BCRYPT_KEY_HANDLE key = nullptr;
  if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_ECDSA_P256_ALGORITHM,
                                  nullptr, 0) < 0)
    return false;
  const auto imported = BCryptImportKeyPair(
      provider, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
      reinterpret_cast<PUCHAR>(native_key.data()),
      static_cast<ULONG>(native_key.size()), 0) >= 0;
  const auto valid = imported &&
                     BCryptVerifySignature(
                         key, nullptr,
                         reinterpret_cast<PUCHAR>(
                             const_cast<std::byte*>(digest.data())),
                         static_cast<ULONG>(digest.size()),
                         reinterpret_cast<PUCHAR>(
                             const_cast<std::byte*>(signature.data())),
                         static_cast<ULONG>(signature.size()), 0) >= 0;
  if (key) BCryptDestroyKey(key);
  BCryptCloseAlgorithmProvider(provider, 0);
  return valid;
#elif defined(__APPLE__)
  auto point = std::vector<std::byte>(public_key.begin(), public_key.end());
  if (point.size() == 72) {
    point.erase(point.begin(), point.begin() + 8);
    point.insert(point.begin(), std::byte{0x04});
  }
  if (point.size() != 65 || point.front() != std::byte{0x04} ||
      signature.size() != 64)
    return false;
  auto encode_integer = [](std::span<const std::byte> value) {
    while (value.size() > 1 && value.front() == std::byte{0})
      value = value.subspan(1);
    std::vector<std::byte> result{std::byte{0x02}};
    const bool prefix =
        (std::to_integer<unsigned>(value.front()) & 0x80U) != 0;
    result.push_back(static_cast<std::byte>(value.size() + (prefix ? 1 : 0)));
    if (prefix) result.push_back(std::byte{0});
    result.insert(result.end(), value.begin(), value.end());
    return result;
  };
  auto r = encode_integer(signature.first(32));
  auto s = encode_integer(signature.subspan(32));
  std::vector<std::byte> der{std::byte{0x30},
                             static_cast<std::byte>(r.size() + s.size())};
  der.insert(der.end(), r.begin(), r.end());
  der.insert(der.end(), s.begin(), s.end());
  const auto key_data = CFDataCreate(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(point.data()),
      static_cast<CFIndex>(point.size()));
  int bits = 256;
  const auto bits_value = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                         &bits);
  const void* keys[] = {kSecAttrKeyType, kSecAttrKeyClass,
                        kSecAttrKeySizeInBits};
  const void* values[] = {kSecAttrKeyTypeECSECPrimeRandom,
                          kSecAttrKeyClassPublic, bits_value};
  const auto attributes = CFDictionaryCreate(
      kCFAllocatorDefault, keys, values, 3,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFErrorRef creation_error = nullptr;
  const auto key = SecKeyCreateWithData(key_data, attributes, &creation_error);
  const auto digest_data = CFDataCreate(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(digest.data()),
      static_cast<CFIndex>(digest.size()));
  const auto signature_data = CFDataCreate(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(der.data()),
      static_cast<CFIndex>(der.size()));
  CFErrorRef verification_error = nullptr;
  const bool valid =
      key && SecKeyVerifySignature(
                 key, kSecKeyAlgorithmECDSASignatureDigestX962SHA256,
                 digest_data, signature_data, &verification_error);
  if (verification_error) CFRelease(verification_error);
  if (creation_error) CFRelease(creation_error);
  if (signature_data) CFRelease(signature_data);
  if (digest_data) CFRelease(digest_data);
  if (key) CFRelease(key);
  if (attributes) CFRelease(attributes);
  if (bits_value) CFRelease(bits_value);
  if (key_data) CFRelease(key_data);
  return valid;
#else
  auto point = std::vector<std::byte>(public_key.begin(), public_key.end());
  if (point.size() == 72) {
    point.erase(point.begin(), point.begin() + 8);
    point.insert(point.begin(), std::byte{0x04});
  }
  if (point.size() != 65 || point.front() != std::byte{0x04} ||
      signature.size() != 64)
    return false;
  auto encode_integer = [](std::span<const std::byte> value) {
    while (value.size() > 1 && value.front() == std::byte{0})
      value = value.subspan(1);
    std::vector<std::byte> result{std::byte{0x02}};
    const bool prefix =
        (std::to_integer<unsigned>(value.front()) & 0x80U) != 0;
    result.push_back(static_cast<std::byte>(value.size() + (prefix ? 1 : 0)));
    if (prefix) result.push_back(std::byte{0});
    result.insert(result.end(), value.begin(), value.end());
    return result;
  };
  auto r = encode_integer(signature.first(32));
  auto s = encode_integer(signature.subspan(32));
  std::vector<std::byte> der{std::byte{0x30},
                             static_cast<std::byte>(r.size() + s.size())};
  der.insert(der.end(), r.begin(), r.end());
  der.insert(der.end(), s.begin(), s.end());

  EVP_PKEY_CTX* create = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
  EVP_PKEY* key = nullptr;
  char group[] = "prime256v1";
  OSSL_PARAM parameters[] = {
      OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
      OSSL_PARAM_construct_octet_string(
          OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()),
      OSSL_PARAM_construct_end()};
  const bool imported =
      create && EVP_PKEY_fromdata_init(create) > 0 &&
      EVP_PKEY_fromdata(create, &key, EVP_PKEY_PUBLIC_KEY, parameters) > 0;
  EVP_PKEY_CTX* verify = imported ? EVP_PKEY_CTX_new(key, nullptr) : nullptr;
  const bool valid =
      verify && EVP_PKEY_verify_init(verify) > 0 &&
      EVP_PKEY_CTX_set_signature_md(verify, EVP_sha256()) > 0 &&
      EVP_PKEY_verify(
          verify, reinterpret_cast<const unsigned char*>(der.data()),
          der.size(), reinterpret_cast<const unsigned char*>(digest.data()),
          digest.size()) == 1;
  if (verify) EVP_PKEY_CTX_free(verify);
  if (key) EVP_PKEY_free(key);
  if (create) EVP_PKEY_CTX_free(create);
  return valid;
#endif
}

bool permission_increases(const Json& current, const Json& candidate) {
  if (!candidate.is_object()) return false;
  for (const auto& [key, value] : candidate.items()) {
    if (!current.contains(key)) return true;
    const auto& previous = current[key];
    if (value.is_boolean() && value.get<bool>() &&
        (!previous.is_boolean() || !previous.get<bool>()))
      return true;
    if (value.is_array() && previous.is_array()) {
      for (const auto& element : value)
        if (std::ranges::find(previous, element) == previous.end()) return true;
    } else if (value.is_object() && previous.is_object() &&
               permission_increases(previous, value)) {
      return true;
    } else if (value != previous && !value.is_boolean()) {
      return true;
    }
  }
  return false;
}

} // namespace

PackageTrustStore::PackageTrustStore(std::filesystem::path trust_json) {
  try {
    trust_ = Json::parse(tokmon::read_text_file(trust_json), nullptr, true,
                         false);
  } catch (const nlohmann::json::exception& error) {
    throw tokmon::Error("arche.trust.json",
                        "invalid trust store JSON: " +
                            std::string(error.what()));
  }
  if (trust_.value("schema", "") != "org.tokmon.arche.trust/v1" ||
      !trust_.contains("signers") || !trust_["signers"].is_object())
    throw tokmon::Error("arche.trust.schema", "invalid trust store schema");
}

PackageVerificationReport PackageTrustStore::verify(
    const std::filesystem::path& package_root) const {
  const auto root = std::filesystem::weakly_canonical(package_root);
  const auto manifest_path = root / "plugin.json";
  Json manifest;
  try {
    manifest = Json::parse(tokmon::read_text_file(manifest_path), nullptr,
                           true, false);
  } catch (const nlohmann::json::exception& error) {
    throw tokmon::Error("arche.package.manifest",
                        "invalid package manifest: " +
                            std::string(error.what()));
  }
  if (!manifest.contains("signature"))
    throw tokmon::Error("arche.package.unsigned",
                        "package manifest has no signature");
  const auto signature = manifest.at("signature");
  const auto signer = signature.at("signer").get<std::string>();
  const auto found = trust_.at("signers").find(signer);
  if (found == trust_.at("signers").end() ||
      !found->value("enabled", true))
    throw tokmon::Error("arche.package.untrusted",
                        "package signer is not trusted: " + signer);
  const auto algorithm = signature.value("algorithm", "");
  if (algorithm != found->value("algorithm", ""))
    throw tokmon::Error("arche.package.algorithm",
                        "signature algorithm does not match trust root");

  std::vector<std::string> artifact_hashes;
  const auto artifacts = manifest.value("artifacts", Json::array());
  if (!artifacts.is_array())
    throw tokmon::Error("arche.package.artifacts",
                        "package artifacts must be an array");
  for (const auto& artifact : artifacts) {
    const auto relative = artifact.at("path").get<std::string>();
    const auto path = tokmon::canonical_within(root, relative, true);
    if (!std::filesystem::is_regular_file(path) ||
        std::filesystem::is_symlink(path))
      throw tokmon::Error("arche.package.artifact",
                          "package artifact must be a regular non-symlink file");
    const auto actual = tokmon::sha256_hex(tokmon::read_text_file(path));
    const auto expected = artifact.at("sha256").get<std::string>();
    if (actual != expected)
      throw tokmon::Error("arche.package.integrity",
                          "package artifact hash mismatch",
                          {{"path", relative},
                           {"expected", expected}, {"actual", actual}});
    artifact_hashes.push_back(actual);
  }
  if (manifest.contains("entrypoint") &&
      manifest.at("entrypoint").is_object() &&
      manifest.at("entrypoint").contains("path")) {
    const auto entrypoint =
        manifest.at("entrypoint").at("path").get<std::string>();
    const auto covered = std::ranges::any_of(artifacts, [&](const auto& item) {
      return item.value("path", "") == entrypoint;
    });
    if (!covered)
      throw tokmon::Error(
          "arche.package.entrypoint-integrity",
          "package entrypoint must be covered by an artifact SHA-256",
          {{"path", entrypoint}});
  }

  auto unsigned_manifest = manifest;
  unsigned_manifest.erase("signature");
  unsigned_manifest.erase("content_hash");
  const auto package_hash =
      tokmon::canonical_json_sha256(unsigned_manifest);
  if (manifest.value("content_hash", "") != "sha256:" + package_hash)
    throw tokmon::Error("arche.package.content-hash",
                        "manifest content_hash does not match package payload");
  const auto digest = decode_hex(package_hash);
  const auto public_key = decode_base64(found->at("public_key").get<std::string>());
  const auto signature_bytes =
      decode_base64(signature.at("value").get<std::string>());
  if (!verify_signature(algorithm, public_key, digest, signature_bytes))
    throw tokmon::Error("arche.package.signature",
                        "package signature verification failed");

  auto descriptor_document = manifest;
  descriptor_document.erase("signature");
  auto descriptor = parse_plugin_manifest(descriptor_document);
  return {std::move(descriptor), signer, "sha256:" + package_hash,
          std::move(artifact_hashes), true};
}

EvolutionProposal EvolutionProposal::parse(const Json& document) {
  if (document.value("schema", "") !=
      "org.tokmon.arche.evolution-proposal/v1")
    throw tokmon::Error("arche.evolution.schema",
                        "invalid evolution proposal schema");
  EvolutionProposal value;
  value.id = document.at("id").get<std::string>();
  value.instance = document.at("instance").get<std::string>();
  value.package_hash = document.at("package_hash").get<std::string>();
  value.capability_delta =
      document.value("capability_delta", Json::object());
  value.permission_delta =
      document.value("permission_delta", Json::object());
  value.evidence = document.value("evidence", Json::array());
  value.rollback = document.value("rollback", Json::object());
  if (value.id.empty() || value.instance.empty() || !value.evidence.is_array() ||
      !value.rollback.is_object())
    throw tokmon::Error("arche.evolution.proposal",
                        "malformed evolution proposal");
  return value;
}

EvolutionGateReport EvolutionGate::validate(
    const EvolutionProposal& proposal, const PluginDescriptor* current,
    const PackageVerificationReport& candidate,
    bool permission_increase_approved) const {
  EvolutionGateReport report;
  if (!candidate.signature_valid)
    report.rejections.push_back("candidate package is not signature-verified");
  if (proposal.package_hash != candidate.package_hash)
    report.rejections.push_back("proposal package hash does not match candidate");
  if (proposal.rollback.empty() ||
      proposal.rollback.value("strategy", "").empty())
    report.rejections.push_back("rollback strategy is missing");
  if (proposal.evidence.empty())
    report.rejections.push_back("validation evidence is empty");
  for (const auto& evidence : proposal.evidence)
    if (evidence.value("status", "") != "passed")
      report.rejections.push_back("validation evidence did not pass: " +
                                  evidence.value("name", "unnamed"));

  {
    std::set<std::string> existing;
    if (current) {
    for (const auto& provision : current->provides)
      existing.insert(provision.capability);
    }
    std::set<std::string> actual_added;
    for (const auto& provision : candidate.descriptor.provides)
      if (!existing.contains(provision.capability))
        actual_added.insert(provision.capability);
    std::set<std::string> declared_added;
    for (const auto& value : proposal.capability_delta.value(
             "added", std::vector<std::string>{}))
      declared_added.insert(value);
    if (actual_added != declared_added)
      report.rejections.push_back(
          "declared capability delta does not match candidate");
    if (permission_increases(current ? current->permissions : Json::object(),
                             candidate.descriptor.permissions) &&
        !permission_increase_approved)
      report.rejections.push_back("permission increase requires approval");
  }
  report.accepted = report.rejections.empty();
  return report;
}

PackageStore::PackageStore(std::filesystem::path root)
    : root_(std::filesystem::absolute(std::move(root))) {
  std::filesystem::create_directories(root_ / "packages");
  std::filesystem::create_directories(root_ / "staging");
}

std::filesystem::path PackageStore::install_verified(
    const std::filesystem::path& package_root,
    const PackageVerificationReport& report) {
  if (!report.signature_valid)
    throw tokmon::Error("arche.package.unverified",
                        "cannot install an unverified package");
  const auto digest = report.package_hash.starts_with("sha256:")
                          ? report.package_hash.substr(7)
                          : report.package_hash;
  auto manifest = Json::parse(
      tokmon::read_text_file(package_root / "plugin.json"), nullptr, true,
      false);
  auto payload = manifest;
  payload.erase("signature");
  payload.erase("content_hash");
  if (tokmon::canonical_json_sha256(payload) != digest)
    throw tokmon::Error("arche.package.changed",
                        "verified package manifest changed before install");
  for (const auto& artifact : manifest.value("artifacts", Json::array())) {
    const auto path = tokmon::canonical_within(
        package_root, artifact.at("path").get<std::string>(), true);
    if (tokmon::sha256_hex(tokmon::read_text_file(path)) !=
        artifact.at("sha256").get<std::string>())
      throw tokmon::Error("arche.package.changed",
                          "verified package artifact changed before install");
  }
  const auto destination = root_ / "packages" / report.descriptor.id /
                           report.descriptor.version / digest;
  if (std::filesystem::exists(destination)) return destination;
  const auto staging = root_ / "staging" / tokmon::make_uuid();
  std::filesystem::create_directories(staging);
  try {
    std::filesystem::copy(package_root, staging,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::rename(staging, destination);
  } catch (...) {
    std::error_code error;
    std::filesystem::remove_all(staging, error);
    throw;
  }
  return destination;
}

} // namespace arche
