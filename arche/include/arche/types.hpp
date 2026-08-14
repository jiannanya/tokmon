#pragma once

#include <tokmon/common/types.hpp>

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace arche {

using RuntimeId = tokmon::RuntimeId;
using ContextId = tokmon::ContextId;
using FiberId = tokmon::FiberId;
using CompositionEpoch = std::uint64_t;
using Json = tokmon::Json;

enum class FiberState {
  inactive,
  reloading,
  active,
  unloading,
};

[[nodiscard]] std::string_view to_string(FiberState state) noexcept;

struct CapabilityRequirement {
  std::string capability;
  std::string range{"*"};
  bool optional{false};
  std::string interface_hash;

  friend bool operator==(const CapabilityRequirement&,
                         const CapabilityRequirement&) = default;
};

struct CapabilityProvision {
  std::string capability;
  std::string version{"1.0.0"};
  std::string interface_hash;
  bool multiple{false};

  friend bool operator==(const CapabilityProvision&,
                         const CapabilityProvision&) = default;
};

struct PluginDescriptor {
  std::string id;
  std::string version;
  std::string abi{"arche-cpp/1"};
  std::vector<std::string> products;
  std::vector<CapabilityRequirement> requirements;
  std::vector<CapabilityProvision> provides;
  Json permissions{Json::object()};
  Json config_schema{Json::object()};
  std::string content_hash;
};

[[nodiscard]] bool version_satisfies(std::string_view version,
                                     std::string_view range);

void to_json(Json& out, const CapabilityRequirement& value);
void from_json(const Json& in, CapabilityRequirement& value);
void to_json(Json& out, const CapabilityProvision& value);
void from_json(const Json& in, CapabilityProvision& value);
void to_json(Json& out, const PluginDescriptor& value);
void from_json(const Json& in, PluginDescriptor& value);

} // namespace arche
