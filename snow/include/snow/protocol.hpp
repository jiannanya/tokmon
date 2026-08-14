#pragma once

#include <snow/agent.hpp>
#include <snow/artifact.hpp>

#include <arche/runtime.hpp>

#include <istream>
#include <ostream>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>

namespace snow {

class Assembly;

class ProtocolApprovalService final : public ApprovalService {
public:
  using NotificationSink = std::function<void(tokmon::Json)>;
  explicit ProtocolApprovalService(
      std::chrono::milliseconds timeout = std::chrono::minutes(5))
      : timeout_(timeout) {}
  bool approve(const ToolDefinition& tool,
               const tokmon::Json& canonical_arguments,
               std::string_view reason,
               const Details& details = {}) override;
  void set_notification_sink(NotificationSink sink);
  bool resolve(std::string_view approval_id, bool approved);
  void cancel_all();

private:
  struct Pending {
    std::optional<bool> decision;
  };
  std::mutex mutex_;
  std::condition_variable condition_;
  std::map<std::string, Pending, std::less<>> pending_;
  NotificationSink sink_;
  std::chrono::milliseconds timeout_;
};


class ProtocolServer final {
public:
  explicit ProtocolServer(
      Agent& agent, std::shared_ptr<ProtocolApprovalService> approvals = {},
      std::shared_ptr<ArtifactStore> artifacts = {},
      arche::Runtime* runtime = nullptr, Assembly* assembly = nullptr)
      : agent_(&agent), approvals_(std::move(approvals)),
        artifacts_(std::move(artifacts)), runtime_(runtime),
        assembly_(assembly) {}
  void serve(std::istream& input, std::ostream& output);
  [[nodiscard]] tokmon::Json handle(const tokmon::Json& request);

private:
  Agent* agent_;
  std::shared_ptr<ProtocolApprovalService> approvals_;
  std::shared_ptr<ArtifactStore> artifacts_;
  arche::Runtime* runtime_{nullptr};
  Assembly* assembly_{nullptr};
  mutable std::mutex turns_mutex_;
  std::map<std::string, std::stop_source, std::less<>> active_turns_;
};

} // namespace snow
