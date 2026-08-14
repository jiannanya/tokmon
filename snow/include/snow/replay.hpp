#pragma once

#include <snow/surface.hpp>

#include <set>
#include <string>
#include <vector>

namespace snow {

enum class ReplayLevel { transcript, request_reconstruction, control };

struct ValidationIssue {
  std::uint64_t seq{0};
  std::string code;
  std::string message;
  bool fatal{true};
};

struct ValidationReport {
  std::vector<ValidationIssue> issues;
  [[nodiscard]] bool valid() const noexcept;
  void throw_if_invalid() const;
};

class TrajectoryValidator final {
public:
  TrajectoryValidator();
  void register_family(std::string prefix);
  [[nodiscard]] ValidationReport validate(
      const std::vector<TrajectoryEvent>& events) const;

private:
  [[nodiscard]] bool known(std::string_view type) const;
  std::set<std::string, std::less<>> families_;
};

struct ReplayReport {
  ReplayLevel level{ReplayLevel::transcript};
  bool deterministic{true};
  std::vector<std::string> degradations;
  tokmon::Json transcript{tokmon::Json::array()};
  tokmon::Json requests{tokmon::Json::array()};
  tokmon::Json control{tokmon::Json::array()};
  tokmon::Json final_state{tokmon::Json::object()};
};

class ReplayEngine final {
public:
  explicit ReplayEngine(TrajectoryValidator validator = {});
  [[nodiscard]] ReplayReport replay(
      const std::vector<TrajectoryEvent>& events,
      ReplayLevel level = ReplayLevel::control) const;

private:
  TrajectoryValidator validator_;
  SurfaceProjection surface_;
};

[[nodiscard]] std::string_view to_string(ReplayLevel level) noexcept;

} // namespace snow
