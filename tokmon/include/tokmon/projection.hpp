#pragma once

#include <snow/trajectory.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tokmon::desktop {

enum class ItemKind {
  user,
  assistant,
  tool,
  approval,
  artifact,
  diagnostic,
  status,
  error,
};

struct ConversationItem {
  std::string id;
  ItemKind kind{ItemKind::status};
  std::string title;
  std::string content;
  std::string status;
  std::uint64_t source_seq{0};
  tokmon::Json metadata{tokmon::Json::object()};
};

struct ProjectionSnapshot {
  std::vector<ConversationItem> items;
  std::vector<snow::TrajectoryEvent> events;
  std::uint64_t cursor{0};
  std::uint64_t revision{0};
};

class Projection final {
public:
  void apply(const snow::TrajectoryEvent &event);
  void replay(const std::vector<snow::TrajectoryEvent> &events);
  [[nodiscard]] std::vector<ConversationItem> snapshot() const;
  [[nodiscard]] std::vector<snow::TrajectoryEvent> event_snapshot() const;
  [[nodiscard]] ProjectionSnapshot snapshot_all() const;
  [[nodiscard]] std::uint64_t cursor() const noexcept;
  [[nodiscard]] std::uint64_t revision() const noexcept;
  void append_local(ItemKind kind, std::string title, std::string content,
                    std::string status = "local",
                    tokmon::Json metadata = tokmon::Json::object());
  void begin_fork();
  void clear();

private:
  mutable std::mutex mutex_;
  std::vector<ConversationItem> items_;
  std::vector<snow::TrajectoryEvent> events_;
  std::optional<std::size_t> streaming_assistant_;
  std::uint64_t cursor_{0};
  std::uint64_t revision_{0};
};

} // namespace tokmon::desktop
