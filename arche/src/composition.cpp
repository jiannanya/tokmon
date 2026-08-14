#include <arche/composition.hpp>

#include <tokmon/common/types.hpp>

#include <algorithm>
#include <set>

namespace arche {
namespace {

std::pair<std::string, std::string> split_package(std::string_view package) {
  const auto separator = package.rfind('@');
  if (separator == std::string_view::npos || separator == 0) {
    return {std::string(package), "*"};
  }
  return {std::string(package.substr(0, separator)),
          std::string(package.substr(separator + 1))};
}

std::vector<int> version_parts(std::string_view version) {
  std::vector<int> result;
  std::size_t start = 0;
  while (start < version.size() && result.size() < 3) {
    const auto end = version.find('.', start);
    try {
      result.push_back(
          std::stoi(std::string(version.substr(start, end - start))));
    } catch (...) {
      result.push_back(0);
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  while (result.size() < 3) result.push_back(0);
  return result;
}

std::string package_name(const PluginDescriptor& descriptor) {
  return descriptor.id + "@" + descriptor.version;
}

} // namespace

DesiredComposition DesiredComposition::parse(const Json& document) {
  if (!document.is_object()) {
    throw tokmon::Error("arche.composition.type",
                        "composition must be a JSON object");
  }
  DesiredComposition result;
  result.schema = document.value("schema", result.schema);
  result.id = document.at("id").get<std::string>();
  result.locks = document.value("locks", Json::object());
  std::set<std::string> instances;
  for (const auto& value : document.at("plugins")) {
    CompositionEntry entry;
    entry.instance = value.at("instance").get<std::string>();
    entry.package = value.at("package").get<std::string>();
    entry.realm = value.value("realm", "");
    entry.config = value.value("config", Json::object());
    entry.disabled = value.value("disabled", false);
    if (entry.instance.empty() || entry.package.empty() ||
        !instances.insert(entry.instance).second) {
      throw tokmon::Error("arche.composition.entry",
                          "composition has invalid or duplicate instance");
    }
    result.plugins.push_back(std::move(entry));
  }
  return result;
}

void PluginCatalog::add(std::string id, std::string version,
                        PluginFactory factory) {
  if (id.empty() || version.empty() || !factory) {
    throw tokmon::Error("arche.catalog.entry", "invalid catalog entry");
  }
  auto& entries = entries_[std::move(id)];
  if (std::ranges::any_of(entries, [&](const auto& entry) {
        return entry.version == version;
      })) {
    throw tokmon::Error("arche.catalog.duplicate",
                        "duplicate package version");
  }
  entries.push_back({std::move(version), std::move(factory)});
  std::ranges::sort(entries, [](const auto& left, const auto& right) {
    return version_parts(left.version) > version_parts(right.version);
  });
}

PluginPtr PluginCatalog::create(std::string_view package,
                                const Json& config) const {
  const auto [id, range] = split_package(package);
  const auto iterator = entries_.find(id);
  if (iterator == entries_.end()) {
    throw tokmon::Error("arche.catalog.missing",
                        "package is not installed: " + id);
  }
  for (const auto& entry : iterator->second) {
    if (version_satisfies(entry.version, range)) {
      auto plugin = entry.factory(config);
      if (!plugin || plugin->descriptor().id != id ||
          plugin->descriptor().version != entry.version) {
        throw tokmon::Error("arche.catalog.descriptor",
                            "package factory descriptor mismatch: " + id);
      }
      return plugin;
    }
  }
  throw tokmon::Error("arche.catalog.version",
                      "no installed version satisfies: " +
                          std::string(package));
}

bool PluginCatalog::contains(std::string_view id,
                             std::string_view version) const {
  const auto iterator = entries_.find(id);
  return iterator != entries_.end() &&
         std::ranges::any_of(iterator->second, [&](const auto& entry) {
           return entry.version == version;
         });
}

CompositionReport Reconciler::apply(
    Runtime& runtime, const DesiredComposition& desired,
    std::string_view managed_instance_prefix) const {
  CompositionReport report;
  report.composition_id = desired.id;
  report.epoch_before = runtime.epoch();

  std::map<std::string, CompositionEntry, std::less<>> target;
  for (const auto& entry : desired.plugins) {
    if (entry.disabled) continue;
    if (!managed_instance_prefix.empty() &&
        !entry.instance.starts_with(managed_instance_prefix)) {
      throw tokmon::Error(
          "arche.composition.scope",
          "composition entry escapes its managed instance prefix",
          {{"instance", entry.instance},
           {"prefix", managed_instance_prefix}});
    }
    target.emplace(entry.instance, entry);
  }

  struct Rollback {
    enum class Kind { remove_new, restore_removed, restore_reloaded };
    Kind kind;
    std::string instance;
    PluginPtr plugin;
    Json config;
    std::string realm;
    bool mounted{false};
  };
  std::vector<Rollback> rollback;

  runtime.begin_composition_transaction();
  try {
    for (const auto& current : runtime.fibers()) {
      if (!managed_instance_prefix.empty() &&
          !current.instance.starts_with(managed_instance_prefix))
        continue;
      if (target.contains(current.instance)) continue;
      auto previous = runtime.plugin_for_reconcile(current.instance);
      runtime.uninstall(current.instance);
      rollback.push_back({Rollback::Kind::restore_removed, current.instance,
                          std::move(previous), current.config, current.realm,
                          current.mounted});
      report.actions.push_back(
          {"remove", current.instance, package_name(current.descriptor), ""});
    }

    for (const auto& [instance, entry] : target) {
      const auto current = runtime.fiber(instance);
      if (!current) {
        auto candidate = catalog_.create(entry.package, entry.config);
        runtime.install(instance, candidate, entry.config, entry.realm);
        runtime.mount(instance);
        rollback.push_back(
            {Rollback::Kind::remove_new, instance, {}, {}, {}, false});
        report.actions.push_back(
            {"install", instance, "", package_name(candidate->descriptor())});
        continue;
      }

      const auto [requested_id, requested_range] = split_package(entry.package);
      const bool implementation_changed =
          current->descriptor.id != requested_id ||
          !version_satisfies(current->descriptor.version, requested_range);
      const bool config_changed = current->config != entry.config;
      const bool realm_changed =
          !entry.realm.empty() && current->realm != entry.realm;
      if (implementation_changed || config_changed || realm_changed) {
        auto previous = runtime.plugin_for_reconcile(instance);
        auto candidate = catalog_.create(entry.package, entry.config);
        runtime.reload(instance, candidate, entry.config, entry.realm);
        rollback.push_back(
            {Rollback::Kind::restore_reloaded, instance, std::move(previous),
             current->config, current->realm, current->mounted});
        report.actions.push_back(
            {"replace", instance, package_name(current->descriptor),
             package_name(candidate->descriptor())});
      } else if (!current->mounted) {
        runtime.mount(instance);
        report.actions.push_back({"mount", instance, "", entry.package});
      }
    }
    runtime.settle();
    for (const auto& [instance, _] : target) {
      const auto actual = runtime.fiber(instance);
      if (!actual || !actual->mounted || actual->state != FiberState::active) {
        throw tokmon::Error(
            "arche.composition.health",
            "desired fiber did not become active: " + instance,
            actual ? Json{{"state", to_string(actual->state)},
                          {"error", actual->error}}
                   : Json{{"state", "missing"}});
      }
    }
    runtime.commit_composition_transaction();
  } catch (...) {
    for (auto iterator = rollback.rbegin(); iterator != rollback.rend();
         ++iterator) {
      try {
        if (iterator->kind == Rollback::Kind::remove_new) {
          runtime.uninstall(iterator->instance);
        } else if (iterator->kind == Rollback::Kind::restore_removed) {
          runtime.install(iterator->instance, iterator->plugin,
                          iterator->config, iterator->realm);
          if (iterator->mounted) runtime.mount(iterator->instance);
        } else {
          runtime.reload(iterator->instance, iterator->plugin,
                         iterator->config, iterator->realm);
          if (!iterator->mounted) runtime.unmount(iterator->instance);
        }
      } catch (...) {
      }
    }
    runtime.abort_composition_transaction();
    throw;
  }

  report.epoch_after = runtime.epoch();
  return report;
}

} // namespace arche
