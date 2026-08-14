#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace tokmon {

using Json = nlohmann::json;
using Clock = std::chrono::system_clock;

class Error final : public std::runtime_error {
public:
  Error(std::string code, std::string message, Json details = Json::object());

  [[nodiscard]] const std::string& code() const noexcept { return code_; }
  [[nodiscard]] const Json& details() const noexcept { return details_; }

private:
  std::string code_;
  Json details_;
};

template <typename Tag>
class StrongId final {
public:
  StrongId() = default;
  explicit StrongId(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] const std::string& str() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

  friend bool operator==(const StrongId&, const StrongId&) = default;
  friend auto operator<=>(const StrongId&, const StrongId&) = default;

private:
  std::string value_;
};

struct RuntimeIdTag;
struct ContextIdTag;
struct FiberIdTag;
struct SessionIdTag;
struct RunIdTag;
struct TurnIdTag;
struct StepIdTag;
struct ModelCallIdTag;
struct ToolCallIdTag;
struct TraceIdTag;
struct SpanIdTag;

using RuntimeId = StrongId<RuntimeIdTag>;
using ContextId = StrongId<ContextIdTag>;
using FiberId = StrongId<FiberIdTag>;
using SessionId = StrongId<SessionIdTag>;
using RunId = StrongId<RunIdTag>;
using TurnId = StrongId<TurnIdTag>;
using StepId = StrongId<StepIdTag>;
using ModelCallId = StrongId<ModelCallIdTag>;
using ToolCallId = StrongId<ToolCallIdTag>;
using TraceId = StrongId<TraceIdTag>;
using SpanId = StrongId<SpanIdTag>;

[[nodiscard]] std::string make_uuid();
[[nodiscard]] std::string iso8601(Clock::time_point time = Clock::now());
[[nodiscard]] std::int64_t unix_millis(Clock::time_point time = Clock::now());

template <typename Tag>
void to_json(Json& out, const StrongId<Tag>& id) {
  out = id.str();
}

template <typename Tag>
void from_json(const Json& in, StrongId<Tag>& id) {
  id = StrongId<Tag>(in.get<std::string>());
}

} // namespace tokmon

template <typename Tag>
struct std::hash<tokmon::StrongId<Tag>> {
  std::size_t operator()(const tokmon::StrongId<Tag>& id) const noexcept {
    return std::hash<std::string>{}(id.str());
  }
};

