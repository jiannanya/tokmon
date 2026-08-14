#pragma once

#include <snow/trajectory.hpp>

#include <string>
#include <vector>

namespace snow {

struct SurfaceItem {
  std::string id;
  std::string role;
  tokmon::Json content;
  std::vector<std::uint64_t> source_event_seqs;
};

void to_json(tokmon::Json& out, const SurfaceItem& item);

class SurfaceProjection final {
public:
  [[nodiscard]] std::vector<SurfaceItem> project(
      const std::vector<TrajectoryEvent>& events) const;
  [[nodiscard]] tokmon::Json model_messages(
      const std::vector<TrajectoryEvent>& events) const;
};

} // namespace snow

