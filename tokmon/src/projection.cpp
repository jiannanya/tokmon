#include <tokmon/projection.hpp>

#include <algorithm>
#include <set>

namespace tokmon::desktop {

void Projection::apply(const snow::TrajectoryEvent& event) {
  std::lock_guard lock(mutex_);
  if (event.seq <= cursor_) {
    return;
  }
  cursor_ = event.seq;

  if (event.type == "user/message") {
    items_.push_back({tokmon::make_uuid(), ItemKind::user, "You",
                      event.data.value("content", ""), "committed",
                      event.seq,
                      {{"attachments",
                        event.data.value("attachments", tokmon::Json::array())},
                       {"time", event.time},
                       {"turn_id", event.turn_id ? event.turn_id->str() : ""}}});
  } else if (event.type == "assistant/chunk") {
    if (!streaming_assistant_) {
      streaming_assistant_ = items_.size();
      items_.push_back({tokmon::make_uuid(), ItemKind::assistant, "Snow", "",
                        "streaming", event.seq,
                        {{"time", event.time},
                         {"turn_id",
                          event.turn_id ? event.turn_id->str() : ""}}});
    }
    items_[*streaming_assistant_].content +=
        event.data.value("content", "");
    items_[*streaming_assistant_].source_seq = event.seq;
  } else if (event.type == "assistant/message") {
    const auto content = event.data.value("content", "");
    if (streaming_assistant_) {
      auto& item = items_[*streaming_assistant_];
      item.content = content;
      item.status = "committed";
      item.source_seq = event.seq;
      item.metadata["completed_at"] = event.time;
      streaming_assistant_.reset();
    } else if (!content.empty()) {
      items_.push_back({tokmon::make_uuid(), ItemKind::assistant, "Snow",
                        content, "committed", event.seq,
                        {{"time", event.time},
                         {"completed_at", event.time},
                         {"turn_id",
                          event.turn_id ? event.turn_id->str() : ""}}});
    }
  } else if (event.type == "turn/end") {
    const auto turn_id = event.turn_id ? event.turn_id->str() : "";
    const auto assistant = std::ranges::find_if(
        items_.rbegin(), items_.rend(), [&](const auto& item) {
          return item.kind == ItemKind::assistant &&
                 (turn_id.empty() ||
                  item.metadata.value("turn_id", "") == turn_id);
        });
    if (assistant != items_.rend()) {
      assistant->metadata["elapsed_ms"] =
          event.data.value("elapsed_ms", std::int64_t{0});
      assistant->metadata["completed_at"] = event.time;
      assistant->metadata["reason"] = event.data.value("reason", "completed");
    }
  } else if (event.type == "tool/call") {
    items_.push_back(
        {event.data.value("tool_call_id", tokmon::make_uuid()),
         ItemKind::tool,
         "Tool / " + event.data.value("name", "unknown"),
         event.data.value("arguments", tokmon::Json::object()).dump(2),
         "proposed",
         event.seq});
  } else if (event.type == "tool/result") {
    const auto id = event.data.value("tool_call_id", "");
    const auto iterator = std::ranges::find(items_, id,
                                            &ConversationItem::id);
    if (iterator != items_.end()) {
      iterator->content = event.data.value("content", "");
      iterator->status = event.data.value("status", "error");
      iterator->source_seq = event.seq;
      iterator->metadata =
          event.data.value("metadata", tokmon::Json::object());
      const auto name = event.data.value("name", "unknown");
      if (name == "shell") iterator->title = "Terminal / shell";
      if (name == "write_file") iterator->title = "Diff / write_file";
    } else {
      items_.push_back({id, ItemKind::tool,
                        "Tool / " + event.data.value("name", "unknown"),
                        event.data.value("content", ""),
                        event.data.value("status", "error"), event.seq,
                        event.data.value("metadata", tokmon::Json::object())});
    }
  } else if (event.type == "approval/request") {
    items_.push_back(
        {event.data.value("tool_call_id", tokmon::make_uuid()),
         ItemKind::approval,
         "Approval / " + event.data.value("name", "tool"),
         event.data.value("canonical_arguments",
                          event.data.value("arguments",
                                           tokmon::Json::object()))
             .dump(2),
         "waiting", event.seq});
  } else if (event.type == "approval/result") {
    const auto id = event.data.value("tool_call_id", "");
    const auto iterator = std::ranges::find(items_, id,
                                            &ConversationItem::id);
    if (iterator != items_.end()) {
      iterator->status = event.data.value("approved", false) ? "approved"
                                                              : "denied";
      iterator->source_seq = event.seq;
    }
  } else if (event.type == "artifact/snapshot") {
    const auto artifacts =
        event.data.value("artifacts", tokmon::Json::object());
    items_.push_back({tokmon::make_uuid(), ItemKind::artifact,
                      "Artifact snapshot", artifacts.dump(2), "committed",
                      event.seq, artifacts});
  } else if (event.type == "agent/error" ||
             event.type == "agent/cancelled") {
    items_.push_back({tokmon::make_uuid(), ItemKind::error, "Error",
                      event.data.value("message", "unknown error"), "error",
                      event.seq});
  } else {
    static const std::set<std::string, std::less<>> canonical_internal = {
        "session/header", "session/title", "session/created", "session/forked",
        "session/closed", "session/end", "session/end-seed",
        "run/start", "run/end", "turn/start", "turn/end", "step/start",
        "step/end", "request/header", "request/context", "model/request",
        "assistant/reasoning-chunk", "tool/normalized", "tool/admission",
        "tool/policy-decision", "tool/start", "tool/dispatch",
        "sandbox/plan", "approval/invalidated", "context/compaction",
        "recovery/crash-repair"};
    if (!canonical_internal.contains(event.type)) {
      items_.push_back({tokmon::make_uuid(), ItemKind::status,
                        "Event / " + event.type, event.data.dump(2),
                        event.ignorable ? "ignorable" : "required",
                        event.seq});
    }
  }
}

void Projection::append_local(ItemKind kind, std::string title,
                              std::string content, std::string status,
                              tokmon::Json metadata) {
  std::lock_guard lock(mutex_);
  items_.push_back({tokmon::make_uuid(), kind, std::move(title),
                    std::move(content), std::move(status), cursor_,
                    std::move(metadata)});
}

void Projection::replay(const std::vector<snow::TrajectoryEvent>& events) {
  clear();
  for (const auto& event : events) {
    apply(event);
  }
}

std::vector<ConversationItem> Projection::snapshot() const {
  std::lock_guard lock(mutex_);
  return items_;
}

std::uint64_t Projection::cursor() const noexcept {
  std::lock_guard lock(mutex_);
  return cursor_;
}

void Projection::begin_fork() {
  std::lock_guard lock(mutex_);
  streaming_assistant_.reset();
  cursor_ = 0;
  items_.push_back({tokmon::make_uuid(), ItemKind::status, "Session fork",
                    "A new branch was created from the durable cursor.",
                    "committed", 0});
}

void Projection::clear() {
  std::lock_guard lock(mutex_);
  items_.clear();
  streaming_assistant_.reset();
  cursor_ = 0;
}

} // namespace tokmon::desktop
