#pragma once

#include <white/document.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace white {

struct PaintNode {
  const Node *source{nullptr};
  Rect bounds;
  Style style;
  std::string text;
  float scroll_offset_y{0};
  float content_height{0};
  bool focused{false};
  std::vector<PaintNode> children;
};

class PaintTree final {
public:
  bool sync(const Document &document);
  [[nodiscard]] const PaintNode &root() const noexcept { return root_; }
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] std::size_t node_count() const noexcept { return node_count_; }

private:
  PaintNode root_;
  std::uint64_t revision_{0};
  std::size_t node_count_{0};
};

} // namespace white
