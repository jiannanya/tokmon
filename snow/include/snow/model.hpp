#pragma once

#include <tokmon/common/types.hpp>

#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace snow {

struct ToolCall {
  tokmon::ToolCallId id;
  std::string name;
  tokmon::Json arguments{tokmon::Json::object()};
};

struct ModelUsage {
  std::uint64_t input_tokens{0};
  std::uint64_t output_tokens{0};
  std::uint64_t cached_tokens{0};
};

struct ModelResponse {
  std::string content;
  std::optional<std::string> reasoning;
  std::vector<ToolCall> tool_calls;
  ModelUsage usage;
  std::string finish_reason{"stop"};
  tokmon::Json provider_metadata{tokmon::Json::object()};
};

struct ModelRequest {
  std::string model;
  tokmon::Json messages{tokmon::Json::array()};
  tokmon::Json tools{tokmon::Json::array()};
  tokmon::Json parameters{tokmon::Json::object()};
};

using ModelChunkCallback =
    std::function<void(std::string_view kind, std::string_view chunk)>;

class ModelProvider {
public:
  virtual ~ModelProvider() = default;
  [[nodiscard]] virtual std::string id() const = 0;
  virtual ModelResponse complete(const ModelRequest& request,
                                 ModelChunkCallback on_chunk,
                                 std::stop_token stop = {}) = 0;
};

class ScriptedModelProvider final : public ModelProvider {
public:
  explicit ScriptedModelProvider(std::vector<ModelResponse> responses);
  [[nodiscard]] std::string id() const override { return "scripted"; }
  ModelResponse complete(const ModelRequest& request,
                         ModelChunkCallback on_chunk,
                         std::stop_token stop = {}) override;
  [[nodiscard]] std::vector<ModelRequest> requests() const;

private:
  mutable std::mutex mutex_;
  std::vector<ModelResponse> responses_;
  std::vector<ModelRequest> requests_;
  std::size_t cursor_{0};
};

struct OpenAICompatibleConfig {
  std::string endpoint;
  std::string api_key;
  std::string model;
  tokmon::Json extra_headers{tokmon::Json::object()};
  std::chrono::milliseconds timeout{120000};
  bool stream{true};
};

class OpenAICompatibleProvider final : public ModelProvider {
public:
  explicit OpenAICompatibleProvider(OpenAICompatibleConfig config);
  [[nodiscard]] std::string id() const override {
    return "openai-compatible";
  }
  ModelResponse complete(const ModelRequest& request,
                         ModelChunkCallback on_chunk,
                         std::stop_token stop = {}) override;

private:
  OpenAICompatibleConfig config_;
};

} // namespace snow
