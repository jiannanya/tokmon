#include <tokmon/product_assembly.hpp>

#include <arche/plugin.hpp>

namespace tokmon::desktop {
namespace {

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
    std::shared_ptr<SnowProcessClient> snow_client)
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
  if (snow_client) {
    desired.plugins.push_back(
        {"tokmon.snow", "org.tokmon.desktop.snow-supervisor@1.0.0", "io"});
  }
  report_ = reconciler_->apply(runtime_, desired, "tokmon.");
  projection_ = runtime_.root_context()->require<Projection>(
      "tokmon.session-projection", "^1.0");
  approvals_ = runtime_.root_context()->require<ApprovalCoordinator>(
      "tokmon.approval-presenter", "^1.0");
  if (snow_client) {
    snow_client_ = runtime_.root_context()->require<SnowProcessClient>(
        "tokmon.snow-client", "^1.0");
  }
}

ProductAssembly::~ProductAssembly() {
  snow_client_ = {};
  approvals_ = {};
  projection_ = {};
  for (const auto* instance : {"tokmon.snow", "tokmon.approval",
                               "tokmon.projection"}) {
    try {
      runtime_.uninstall(instance);
    } catch (...) {
    }
  }
}

} // namespace tokmon::desktop
