#pragma once

#include <snow/agent.hpp>

#include <arche/runtime.hpp>
#include <arche/composition.hpp>
#include <arche/package.hpp>

#include <memory>
#include <mutex>

namespace snow {

class Assembly final {
public:
  Assembly(BootstrapConfig config, std::shared_ptr<ModelProvider> model,
           std::shared_ptr<ApprovalService> approvals = {});
  ~Assembly();

  [[nodiscard]] Agent& agent();
  [[nodiscard]] arche::Runtime& arche_runtime() noexcept { return runtime_; }
  [[nodiscard]] const BootstrapConfig& config() const noexcept {
    return config_;
  }
  [[nodiscard]] const arche::CompositionReport& composition_report() const
      noexcept { return composition_report_; }
  [[nodiscard]] tokmon::Json stage_package(
      const tokmon::SessionId& session,
      const std::filesystem::path& package_root,
      const arche::EvolutionProposal& proposal,
      bool permission_increase_approved = false);
  [[nodiscard]] arche::CompositionReport apply_composition(
      const tokmon::SessionId& session,
      const arche::DesiredComposition& desired);

private:
  BootstrapConfig config_;
  arche::Runtime runtime_{"snow"};
  arche::PluginCatalog catalog_;
  std::unique_ptr<arche::Reconciler> reconciler_;
  arche::CompositionReport composition_report_;
  arche::CapabilityLease<Agent> agent_;
  mutable std::mutex composition_mutex_;
};

} // namespace snow
