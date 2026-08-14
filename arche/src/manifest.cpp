#include <arche/manifest.hpp>

#include <tokmon/common/files.hpp>

#include <charconv>
#include <regex>
#include <set>
#include <tuple>

namespace arche {

namespace {

std::tuple<int, int, int> parse_version(std::string_view value) {
  int major = 0;
  int minor = 0;
  int patch = 0;
  const std::regex expression(
      R"(^([0-9]+)(?:\.([0-9]+))?(?:\.([0-9]+))?(?:[-+].*)?$)");
  std::cmatch match;
  const std::string owned(value);
  if (!std::regex_match(owned.c_str(), match, expression)) {
    return {-1, -1, -1};
  }
  major = std::stoi(match[1].str());
  if (match[2].matched) {
    minor = std::stoi(match[2].str());
  }
  if (match[3].matched) {
    patch = std::stoi(match[3].str());
  }
  return {major, minor, patch};
}

} // namespace

std::string_view to_string(FiberState state) noexcept {
  switch (state) {
  case FiberState::inactive:
    return "inactive";
  case FiberState::reloading:
    return "reloading";
  case FiberState::active:
    return "active";
  case FiberState::unloading:
    return "unloading";
  }
  return "unknown";
}

bool version_satisfies(std::string_view version, std::string_view range) {
  if (range.empty() || range == "*") {
    return true;
  }
  const auto parsed = parse_version(version);
  if (std::get<0>(parsed) < 0) {
    return false;
  }
  if (range.starts_with('^')) {
    const auto required = parse_version(range.substr(1));
    if (std::get<0>(required) < 0) {
      return false;
    }
    return std::get<0>(parsed) == std::get<0>(required) &&
           parsed >= required;
  }
  if (range.starts_with("~")) {
    const auto required = parse_version(range.substr(1));
    return std::get<0>(parsed) == std::get<0>(required) &&
           std::get<1>(parsed) == std::get<1>(required) &&
           parsed >= required;
  }
  if (range.starts_with(">=")) {
    return parsed >= parse_version(range.substr(2));
  }
  return parsed == parse_version(range);
}

void to_json(Json& out, const CapabilityRequirement& value) {
  out = {{"capability", value.capability},
         {"range", value.range},
         {"optional", value.optional},
         {"interface_hash", value.interface_hash}};
}

void from_json(const Json& in, CapabilityRequirement& value) {
  in.at("capability").get_to(value.capability);
  value.range = in.value("range", "*");
  value.optional = in.value("optional", false);
  value.interface_hash = in.value("interface_hash", "");
}

void to_json(Json& out, const CapabilityProvision& value) {
  out = {{"capability", value.capability},
         {"version", value.version},
         {"interface_hash", value.interface_hash},
         {"multiple", value.multiple}};
}

void from_json(const Json& in, CapabilityProvision& value) {
  in.at("capability").get_to(value.capability);
  value.version = in.value("version", "1.0.0");
  value.interface_hash = in.value("interface_hash", "");
  value.multiple = in.value("multiple", false);
}

void to_json(Json& out, const PluginDescriptor& value) {
  out = {{"id", value.id},
         {"version", value.version},
         {"abi", value.abi},
         {"products", value.products},
         {"requires", value.requirements},
         {"provides", value.provides},
         {"permissions", value.permissions},
         {"config_schema", value.config_schema},
         {"content_hash", value.content_hash}};
}

void from_json(const Json& in, PluginDescriptor& value) {
  in.at("id").get_to(value.id);
  in.at("version").get_to(value.version);
  value.abi = in.value("abi", "arche-cpp/1");
  if (in.contains("host")) {
    value.products = in["host"].value("products", std::vector<std::string>{});
  } else {
    value.products = in.value("products", std::vector<std::string>{});
  }
  value.requirements =
      in.value("requires", std::vector<CapabilityRequirement>{});
  value.provides =
      in.value("provides", std::vector<CapabilityProvision>{});
  value.permissions = in.value("permissions", Json::object());
  value.config_schema = in.value("config_schema", Json::object());
  if (value.config_schema.is_string()) {
    value.config_schema = Json{{"$ref", value.config_schema}};
  }
  value.content_hash = in.value("content_hash", "");
  if (value.content_hash.empty() && in.contains("artifacts")) {
    value.content_hash = in["artifacts"].value("hash", "");
  }
}

void validate_plugin_descriptor(const PluginDescriptor& descriptor) {
  static const std::regex id_pattern(
      R"(^[a-z0-9](?:[a-z0-9._-]*[a-z0-9])?$)");
  if (!std::regex_match(descriptor.id, id_pattern)) {
    throw tokmon::Error("arche.manifest.id",
                        "invalid plugin id: " + descriptor.id);
  }
  if (std::get<0>(parse_version(descriptor.version)) < 0) {
    throw tokmon::Error("arche.manifest.version",
                        "invalid plugin version: " + descriptor.version);
  }
  std::set<std::string> provisions;
  for (const auto& provision : descriptor.provides) {
    if (provision.capability.empty() ||
        !provisions.insert(provision.capability).second) {
      throw tokmon::Error("arche.manifest.provision",
                          "duplicate or empty capability provision");
    }
    if (std::get<0>(parse_version(provision.version)) < 0) {
      throw tokmon::Error("arche.manifest.provision_version",
                          "invalid capability version: " + provision.version);
    }
  }
  for (const auto& requirement : descriptor.requirements) {
    if (requirement.capability.empty()) {
      throw tokmon::Error("arche.manifest.requirement",
                          "empty capability requirement");
    }
  }
}

PluginDescriptor parse_plugin_manifest(const Json& document) {
  if (!document.is_object()) {
    throw tokmon::Error("arche.manifest.type",
                        "plugin manifest must be a JSON object");
  }
  auto descriptor = document.get<PluginDescriptor>();
  validate_plugin_descriptor(descriptor);
  return descriptor;
}

PluginDescriptor load_plugin_manifest(const std::filesystem::path& path) {
  try {
    return parse_plugin_manifest(
        Json::parse(tokmon::read_text_file(path), nullptr, true, false));
  } catch (const nlohmann::json::exception& error) {
    throw tokmon::Error("arche.manifest.json",
                        "invalid plugin JSON: " + std::string(error.what()));
  }
}

} // namespace arche
