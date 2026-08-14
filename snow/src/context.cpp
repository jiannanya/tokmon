#include <snow/context.hpp>

#include <tokmon/common/files.hpp>
#include <tokmon/common/digest.hpp>

#include <algorithm>

namespace snow {
namespace {

std::string digest(std::string_view value) {
  return "sha256:" + tokmon::sha256_hex(value);
}

void add_file(std::vector<ContextContribution>& result, std::string kind,
              const std::filesystem::path& root,
              const std::filesystem::path& path, bool sensitive = false) {
  if (!std::filesystem::is_regular_file(path)) {
    return;
  }
  auto content = tokmon::read_text_file(path);
  constexpr std::size_t maximum = 256 * 1024;
  if (content.size() > maximum) {
    content.resize(maximum);
    content += "\n...[truncated by Snow context source]";
  }
  result.push_back(
      {std::move(kind),
       std::filesystem::relative(path, root).generic_string(),
       content,
       digest(content),
       content.size(),
       sensitive});
}

} // namespace

std::vector<ContextContribution> ContextSources::collect() const {
  std::vector<ContextContribution> result;
  add_file(result, "instructions", layout_.workspace, layout_.instructions);

  std::error_code error;
  if (std::filesystem::is_directory(layout_.skills, error)) {
    std::vector<std::filesystem::path> skills;
    for (const auto& entry :
         std::filesystem::directory_iterator(layout_.skills, error)) {
      if (entry.is_directory(error)) {
        const auto skill = entry.path() / "SKILL.md";
        if (std::filesystem::is_regular_file(skill, error)) {
          skills.push_back(skill);
        }
      } else if (entry.path().filename() == "SKILL.md") {
        skills.push_back(entry.path());
      }
    }
    std::ranges::sort(skills);
    for (const auto& skill : skills) {
      add_file(result, "skill", layout_.workspace, skill);
    }
  }

  if (std::filesystem::is_directory(layout_.memory, error)) {
    std::vector<std::filesystem::path> memories;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(
             layout_.memory,
             std::filesystem::directory_options::skip_permission_denied,
             error)) {
      if (entry.is_regular_file(error) && entry.path().extension() == ".md") {
        memories.push_back(entry.path());
      }
    }
    std::ranges::sort(memories);
    for (const auto& memory : memories) {
      add_file(result, "memory", layout_.workspace, memory, true);
    }
  }
  return result;
}

std::string ContextSources::system_prompt(
    const std::vector<ContextContribution>& contributions) const {
  std::string prompt =
      "You are Snow, an agent operating inside the configured workspace. "
      "Use tools when needed and respect policy, approval, and sandbox "
      "decisions.\n";
  for (const auto& contribution : contributions) {
    prompt += "\n--- " + contribution.kind + ": " + contribution.source +
              " ---\n" + contribution.content + "\n";
  }
  return prompt;
}

tokmon::Json ContextSources::provenance(
    const std::vector<ContextContribution>& contributions) const {
  tokmon::Json result = tokmon::Json::array();
  for (const auto& contribution : contributions) {
    result.push_back({{"kind", contribution.kind},
                      {"source", contribution.source},
                      {"hash", contribution.hash},
                      {"bytes", contribution.bytes},
                      {"sensitive", contribution.sensitive}});
  }
  return result;
}

} // namespace snow
