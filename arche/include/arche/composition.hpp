#pragma once

#include <arche/runtime.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace arche {

struct CompositionEntry {
  std::string instance;
  std::string package;
  std::string realm;
  Json config{Json::object()};
  bool disabled{false};
};

struct DesiredComposition {
  std::string schema{"org.tokmon.arche.composition/v1"};
  std::string id;
  std::vector<CompositionEntry> plugins;
  Json locks{Json::object()};

  [[nodiscard]] static DesiredComposition parse(const Json& document);
};

using PluginFactory = std::function<PluginPtr(const Json& config)>;

class PluginCatalog final {
public:
  void add(std::string id, std::string version, PluginFactory factory);
  [[nodiscard]] PluginPtr create(std::string_view package,
                                 const Json& config) const;
  [[nodiscard]] bool contains(std::string_view id,
                              std::string_view version) const;

private:
  struct Entry {
    std::string version;
    PluginFactory factory;
  };
  std::map<std::string, std::vector<Entry>, std::less<>> entries_;
};

struct CompositionAction {
  std::string action;
  std::string instance;
  std::string from;
  std::string to;
};

struct CompositionReport {
  std::string composition_id;
  CompositionEpoch epoch_before{0};
  CompositionEpoch epoch_after{0};
  std::vector<CompositionAction> actions;
};

class Reconciler final {
public:
  explicit Reconciler(const PluginCatalog& catalog) : catalog_(catalog) {}
  CompositionReport apply(Runtime& runtime,
                          const DesiredComposition& desired,
                          std::string_view managed_instance_prefix = {}) const;

private:
  const PluginCatalog& catalog_;
};

} // namespace arche
