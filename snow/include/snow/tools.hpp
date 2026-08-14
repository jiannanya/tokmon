#pragma once

#include <snow/artifact.hpp>
#include <snow/config.hpp>
#include <snow/model.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace snow {

enum class ToolDisposition {
  allow,
  ask,
  deny,
};

struct ToolDefinition {
  std::string name;
  std::string description;
  tokmon::Json input_schema;
  bool parallel_safe{false};
  bool read_only{false};
};

struct ToolResult {
  bool success{false};
  std::string status{"error"};
  std::string content;
  tokmon::Json metadata{tokmon::Json::object()};
};

struct CanonicalToolPlan {
  std::string tool;
  tokmon::Json arguments{tokmon::Json::object()};
  std::string hash;
  std::string idempotency_key;
  bool read_only{false};
  bool parallel_safe{false};
};

[[nodiscard]] CanonicalToolPlan normalize_tool_plan(
    const ToolDefinition& definition, const tokmon::Json& arguments,
    const tokmon::ToolCallId& call_id);

class Tool {
public:
  virtual ~Tool() = default;
  [[nodiscard]] virtual const ToolDefinition& definition() const = 0;
  virtual ToolResult execute(const tokmon::Json& arguments,
                             std::stop_token stop) = 0;
};

class ToolRegistry final {
public:
  void add(std::shared_ptr<Tool> tool);
  void remove(std::string_view name);
  [[nodiscard]] std::shared_ptr<Tool> find(std::string_view name) const;
  [[nodiscard]] std::vector<ToolDefinition> definitions() const;
  [[nodiscard]] tokmon::Json model_schema() const;

private:
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<Tool>, std::less<>> tools_;
};

class PolicyEngine {
public:
  virtual ~PolicyEngine() = default;
  [[nodiscard]] virtual ToolDisposition decide(
      const ToolDefinition& tool, const tokmon::Json& arguments,
      std::string* reason) const = 0;
};

class DefaultPolicy final : public PolicyEngine {
public:
  explicit DefaultPolicy(bool non_interactive = false)
      : non_interactive_(non_interactive) {}
  ToolDisposition decide(const ToolDefinition& tool,
                         const tokmon::Json& arguments,
                         std::string* reason) const override;

private:
  bool non_interactive_{false};
};

class JsonPolicyEngine final : public PolicyEngine {
public:
  explicit JsonPolicyEngine(tokmon::Json policy);
  ToolDisposition decide(const ToolDefinition& tool,
                         const tokmon::Json& arguments,
                         std::string* reason) const override;

private:
  tokmon::Json policy_;
};

class ApprovalService {
public:
  struct Details {
    std::string canonical_plan_hash;
    std::string idempotency_key;
    tokmon::Json sandbox_plan{tokmon::Json::object()};
  };

  virtual ~ApprovalService() = default;
  virtual bool approve(const ToolDefinition& tool,
                       const tokmon::Json& canonical_arguments,
                       std::string_view reason,
                       const Details& details = {}) = 0;
};

class CallbackApproval final : public ApprovalService {
public:
  using Callback = std::function<bool(const ToolDefinition&,
                                      const tokmon::Json&, std::string_view)>;
  explicit CallbackApproval(Callback callback)
      : callback_(std::move(callback)) {}
  bool approve(const ToolDefinition& tool,
               const tokmon::Json& canonical_arguments,
               std::string_view reason,
               const Details& = {}) override {
    return callback_(tool, canonical_arguments, reason);
  }

private:
  Callback callback_;
};

class ReadFileTool final : public Tool {
public:
  explicit ReadFileTool(std::filesystem::path workspace);
  const ToolDefinition& definition() const override { return definition_; }
  ToolResult execute(const tokmon::Json& arguments,
                     std::stop_token stop) override;

private:
  std::filesystem::path workspace_;
  ToolDefinition definition_;
};

class WriteFileTool final : public Tool {
public:
  explicit WriteFileTool(std::filesystem::path workspace,
                         std::shared_ptr<ArtifactStore> artifacts = {});
  const ToolDefinition& definition() const override { return definition_; }
  ToolResult execute(const tokmon::Json& arguments,
                     std::stop_token stop) override;

private:
  std::filesystem::path workspace_;
  std::shared_ptr<ArtifactStore> artifacts_;
  ToolDefinition definition_;
};

class SearchFilesTool final : public Tool {
public:
  explicit SearchFilesTool(std::filesystem::path workspace);
  const ToolDefinition& definition() const override { return definition_; }
  ToolResult execute(const tokmon::Json& arguments,
                     std::stop_token stop) override;

private:
  std::filesystem::path workspace_;
  ToolDefinition definition_;
};

class ShellTool final : public Tool {
public:
  explicit ShellTool(std::filesystem::path workspace);
  const ToolDefinition& definition() const override { return definition_; }
  ToolResult execute(const tokmon::Json& arguments,
                     std::stop_token stop) override;

private:
  std::filesystem::path workspace_;
  ToolDefinition definition_;
};

} // namespace snow
