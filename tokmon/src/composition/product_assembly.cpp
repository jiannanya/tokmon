#include <tokmon/product_assembly.hpp>
#include <tokmon/workbench.hpp>
#include <tokmon/workbench_boundaries.hpp>

#include <arche/plugin.hpp>
#include <white/native_component.hpp>

namespace tokmon::desktop {
namespace {

arche::CapabilityRequirement requirement(std::string capability,
                                         std::string interface_hash);

template <typename Service>
class ProductServicePlugin final : public arche::Plugin {
public:
  ProductServicePlugin(
      std::string id, std::string capability, std::string interface_hash,
      std::shared_ptr<Service> service,
      std::vector<arche::CapabilityRequirement> requirements = {})
      : capability_(std::move(capability)), service_(std::move(service)) {
    descriptor_.id = std::move(id);
    descriptor_.version = "1.0.0";
    descriptor_.requirements = std::move(requirements);
    descriptor_.provides.push_back(
        {capability_, "1.0.0", std::move(interface_hash), false});
  }
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context& context) override {
    context.provide<Service>(capability_, "1.0.0", service_);
  }

private:
  arche::PluginDescriptor descriptor_;
  std::string capability_;
  std::shared_ptr<Service> service_;
};

class WorkbenchPlugin final : public arche::Plugin {
public:
  explicit WorkbenchPlugin(std::shared_ptr<WorkbenchView> workbench)
      : workbench_(std::move(workbench)) {
    descriptor_.id = "org.tokmon.desktop.workbench";
    descriptor_.version = "1.0.0";
    descriptor_.requirements.push_back(
        requirement("white.native-components", "white-native-components-v1"));
    descriptor_.provides.push_back(
        {"tokmon.workbench", "1.0.0", "tokmon-workbench-v2", false});
  }

  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }

  void apply(arche::Context& context) override {
    auto registry = context
                        .require<white::NativeComponentRegistry>(
                            "white.native-components", "^1.0")
                        .shared();
    register_workbench_boundaries(*registry);
    context.on_unload("tokmon.workbench.native-components",
                      [registry] {
                        unregister_workbench_boundaries(*registry);
                      });
    context.provide<WorkbenchView>("tokmon.workbench", "1.0.0", workbench_);
  }

private:
  arche::PluginDescriptor descriptor_;
  std::shared_ptr<WorkbenchView> workbench_;
};

arche::CapabilityRequirement requirement(std::string capability,
                                         std::string interface_hash) {
  arche::CapabilityRequirement value;
  value.capability = std::move(capability);
  value.range = "^1.0";
  value.interface_hash = std::move(interface_hash);
  return value;
}

} // namespace

ProductAssembly::ProductAssembly(
    arche::Runtime& runtime, std::shared_ptr<Projection> projection,
    std::shared_ptr<ApprovalCoordinator> approvals,
    std::shared_ptr<SnowProcessClient> snow_client,
    std::shared_ptr<WorkbenchView> workbench)
    : runtime_(runtime) {
  catalog_.add("org.tokmon.desktop.session-projection", "1.0.0",
               [projection](const arche::Json&) {
                 return std::make_shared<ProductServicePlugin<Projection>>(
                     "org.tokmon.desktop.session-projection",
                     "tokmon.session-projection", "tokmon-projection-v1",
                     projection);
               });
  catalog_.add("org.tokmon.desktop.approval-presenter", "1.0.0",
               [approvals](const arche::Json&) {
                 return std::make_shared<
                     ProductServicePlugin<ApprovalCoordinator>>(
                     "org.tokmon.desktop.approval-presenter",
                     "tokmon.approval-presenter", "tokmon-approval-v1",
                     approvals);
               });
  if (workbench) {
    catalog_.add("org.tokmon.desktop.workbench", "1.0.0",
                 [workbench](const arche::Json&) {
                   return std::make_shared<WorkbenchPlugin>(workbench);
                 });
  }
  if (snow_client) {
    catalog_.add("org.tokmon.desktop.snow-supervisor", "1.0.0",
                 [snow_client](const arche::Json&) {
                   return std::make_shared<
                       ProductServicePlugin<SnowProcessClient>>(
                       "org.tokmon.desktop.snow-supervisor",
                       "tokmon.snow-client", "tokmon-snow-client-v1",
                       snow_client,
                       std::vector{
                           requirement("tokmon.session-projection",
                                       "tokmon-projection-v1"),
                           requirement("tokmon.approval-presenter",
                                       "tokmon-approval-v1")});
                 });
  }
  reconciler_ = std::make_unique<arche::Reconciler>(catalog_);
  arche::DesiredComposition desired;
  desired.id = "org.tokmon.desktop.default";
  desired.plugins = {
      {"tokmon.projection",
       "org.tokmon.desktop.session-projection@1.0.0", "ui"},
      {"tokmon.approval",
       "org.tokmon.desktop.approval-presenter@1.0.0", "ui"}};
  if (workbench) {
    desired.plugins.push_back(
        {"tokmon.workbench", "org.tokmon.desktop.workbench@1.0.0", "ui"});
  }
  if (snow_client) {
    desired.plugins.push_back(
        {"tokmon.snow", "org.tokmon.desktop.snow-supervisor@1.0.0", "io"});
  }
  report_ = reconciler_->apply(runtime_, desired, "tokmon.");
  projection_ = runtime_.root_context()->require<Projection>(
      "tokmon.session-projection", "^1.0");
  approvals_ = runtime_.root_context()->require<ApprovalCoordinator>(
      "tokmon.approval-presenter", "^1.0");
  if (workbench) {
    workbench_ = runtime_.root_context()->require<WorkbenchView>(
        "tokmon.workbench", "^1.0");
  }
  if (snow_client) {
    snow_client_ = runtime_.root_context()->require<SnowProcessClient>(
        "tokmon.snow-client", "^1.0");
  }
}

ProductAssembly::~ProductAssembly() {
  workbench_ = {};
  snow_client_ = {};
  approvals_ = {};
  projection_ = {};
  for (const auto* instance : {"tokmon.snow", "tokmon.workbench",
                               "tokmon.approval",
                               "tokmon.projection"}) {
    try {
      runtime_.uninstall(instance);
    } catch (...) {
    }
  }
}

} // namespace tokmon::desktop
