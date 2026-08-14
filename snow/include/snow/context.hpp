#pragma once

#include <snow/config.hpp>

#include <string>
#include <vector>

namespace snow {

struct ContextContribution {
  std::string kind;
  std::string source;
  std::string content;
  std::string hash;
  std::size_t bytes{0};
  bool sensitive{false};
};

class ContextSources final {
public:
  explicit ContextSources(ConfigLayout layout) : layout_(std::move(layout)) {}

  [[nodiscard]] std::vector<ContextContribution> collect() const;
  [[nodiscard]] std::string system_prompt(
      const std::vector<ContextContribution>& contributions) const;
  [[nodiscard]] tokmon::Json provenance(
      const std::vector<ContextContribution>& contributions) const;

private:
  ConfigLayout layout_;
};

} // namespace snow

