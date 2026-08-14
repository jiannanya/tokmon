#include <tokmon/approval.hpp>

namespace tokmon::desktop {

bool ApprovalCoordinator::approve(
    const snow::ToolDefinition& tool,
    const tokmon::Json& canonical_arguments, std::string_view reason,
    const Details& details) {
  std::unique_lock lock(mutex_);
  if (cancelled_) {
    return false;
  }
  pending_ = PendingApproval{tokmon::make_uuid(), tool, canonical_arguments,
                             std::string(reason), details};
  remote_responder_ = {};
  decision_.reset();
  const auto changed = changed_;
  lock.unlock();
  if (changed) changed();
  lock.lock();
  ready_.wait(lock, [this] { return decision_.has_value() || cancelled_; });
  const auto result = decision_.value_or(false);
  pending_.reset();
  decision_.reset();
  lock.unlock();
  if (changed) changed();
  return result;
}

std::optional<PendingApproval> ApprovalCoordinator::pending() const {
  std::lock_guard lock(mutex_);
  return pending_;
}

void ApprovalCoordinator::present(
    PendingApproval approval,
    std::function<void(std::string, bool)> responder) {
  std::function<void()> changed;
  {
    std::lock_guard lock(mutex_);
    if (cancelled_) return;
    pending_ = std::move(approval);
    decision_.reset();
    remote_responder_ = std::move(responder);
    changed = changed_;
  }
  if (changed) changed();
}

void ApprovalCoordinator::clear(std::string_view approval_id) {
  std::function<void()> changed;
  {
    std::lock_guard lock(mutex_);
    if (!pending_ || pending_->id != approval_id) return;
    pending_.reset();
    remote_responder_ = {};
    changed = changed_;
  }
  if (changed) changed();
}

bool ApprovalCoordinator::resolve(bool approved) {
  std::function<void(std::string, bool)> responder;
  std::string approval_id;
  {
    std::lock_guard lock(mutex_);
    if (!pending_) return false;
    if (remote_responder_) {
      responder = remote_responder_;
      approval_id = pending_->id;
      pending_.reset();
      remote_responder_ = {};
    } else {
      decision_ = approved;
    }
  }
  if (responder) {
    responder(std::move(approval_id), approved);
    std::function<void()> changed;
    {
      std::lock_guard lock(mutex_);
      changed = changed_;
    }
    if (changed) changed();
  } else {
    ready_.notify_all();
  }
  return true;
}

void ApprovalCoordinator::cancel() {
  {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    decision_ = false;
    remote_responder_ = {};
  }
  ready_.notify_all();
}

void ApprovalCoordinator::set_changed(std::function<void()> changed) {
  std::lock_guard lock(mutex_);
  changed_ = std::move(changed);
}

} // namespace tokmon::desktop
