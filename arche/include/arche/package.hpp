#pragma once

#include <arche/manifest.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace arche {

struct PackageVerificationReport {
  PluginDescriptor descriptor;
  std::string signer;
  std::string package_hash;
  std::vector<std::string> artifact_hashes;
  bool signature_valid{false};
};

class PackageTrustStore final {
public:
  explicit PackageTrustStore(std::filesystem::path trust_json);
  [[nodiscard]] PackageVerificationReport verify(
      const std::filesystem::path& package_root) const;

private:
  Json trust_;
};

struct EvolutionProposal {
  std::string id;
  std::string instance;
  std::string package_hash;
  Json capability_delta{Json::object()};
  Json permission_delta{Json::object()};
  Json evidence{Json::array()};
  Json rollback{Json::object()};

  [[nodiscard]] static EvolutionProposal parse(const Json& document);
};

struct EvolutionGateReport {
  bool accepted{false};
  std::vector<std::string> rejections;
};

class EvolutionGate final {
public:
  [[nodiscard]] EvolutionGateReport validate(
      const EvolutionProposal& proposal,
      const PluginDescriptor* current,
      const PackageVerificationReport& candidate,
      bool permission_increase_approved = false) const;
};

class PackageStore final {
public:
  explicit PackageStore(std::filesystem::path root);
  [[nodiscard]] std::filesystem::path install_verified(
      const std::filesystem::path& package_root,
      const PackageVerificationReport& report);

private:
  std::filesystem::path root_;
};

} // namespace arche
