#pragma once

#include <tokmon/approval.hpp>
#include <tokmon/projection.hpp>
#include <tokmon/snow_client.hpp>

#include <arche/composition.hpp>

namespace tokmon::desktop {

class WorkbenchView;

// Product capabilities are composed into the same Arche runtime as White, but
// own only the tokmon.* instance namespace. This is a spatial child
// composition, not a second plugin manager.
class ProductAssembly final {
public:
  ProductAssembly(arche::Runtime& runtime,
                  std::shared_ptr<Projection> projection,
                  std::shared_ptr<ApprovalCoordinator> approvals,
                  std::shared_ptr<SnowProcessClient> snow_client = {},
                  std::shared_ptr<WorkbenchView> workbench = {});
  ~ProductAssembly();

  [[nodiscard]] const arche::CompositionReport& report() const noexcept {
    return report_;
  }

private:
  arche::Runtime& runtime_;
  arche::PluginCatalog catalog_;
  std::unique_ptr<arche::Reconciler> reconciler_;
  arche::CompositionReport report_;
  arche::CapabilityLease<Projection> projection_;
  arche::CapabilityLease<ApprovalCoordinator> approvals_;
  arche::CapabilityLease<SnowProcessClient> snow_client_;
  arche::CapabilityLease<WorkbenchView> workbench_;
};

} // namespace tokmon::desktop
