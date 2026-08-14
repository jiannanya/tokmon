#pragma once

#include <snow/tools.hpp>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

namespace tokmon::desktop {

struct PendingApproval {
  std::string id;
  snow::ToolDefinition tool;
  tokmon::Json arguments;
  std::string reason;
  snow::ApprovalService::Details details;
};

class ApprovalCoordinator final : public snow::ApprovalService {
public:
  bool approve(const snow::ToolDefinition& tool,
               const tokmon::Json& canonical_arguments,
               std::string_view reason,
               const Details& details = {}) override;

  [[nodiscard]] std::optional<PendingApproval> pending() const;
  // Presents an approval originating in a child Snow process. Resolution is
  // forwarded asynchronously by the supplied responder instead of blocking a
  // local Agent call.
  void present(PendingApproval approval,
               std::function<void(std::string, bool)> responder);
  void clear(std::string_view approval_id);
  bool resolve(bool approved);
  void cancel();
  void set_changed(std::function<void()> changed);

private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<PendingApproval> pending_;
  std::optional<bool> decision_;
  std::function<void(std::string, bool)> remote_responder_;
  bool cancelled_{false};
  std::function<void()> changed_;
};

} // namespace tokmon::desktop
