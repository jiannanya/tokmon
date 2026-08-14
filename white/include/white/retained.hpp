#pragma once

#include <white/types.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace white {

enum class Invalidation : std::uint8_t {
  none = 0,
  style = 1U << 0U,
  layout = 1U << 1U,
  paint = 1U << 2U,
  tree = 1U << 3U,
};

constexpr Invalidation operator|(Invalidation left,
                                 Invalidation right) noexcept {
  return static_cast<Invalidation>(static_cast<std::uint8_t>(left) |
                                   static_cast<std::uint8_t>(right));
}
constexpr Invalidation operator&(Invalidation left,
                                 Invalidation right) noexcept {
  return static_cast<Invalidation>(static_cast<std::uint8_t>(left) &
                                   static_cast<std::uint8_t>(right));
}
constexpr Invalidation &operator|=(Invalidation &left,
                                   Invalidation right) noexcept {
  left = left | right;
  return left;
}
[[nodiscard]] constexpr bool any(Invalidation value) noexcept {
  return value != Invalidation::none;
}

class DamageRegion final {
public:
  void add(Rect rect);
  void merge(const DamageRegion &other);
  void mark_full() noexcept;
  void clear() noexcept;

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] bool full() const noexcept { return full_; }
  [[nodiscard]] bool intersects(const Rect &rect) const noexcept;
  [[nodiscard]] Rect bounds() const noexcept;
  [[nodiscard]] std::span<const Rect> rects() const noexcept { return rects_; }

private:
  bool full_{false};
  std::vector<Rect> rects_;
};

struct FrameMetrics {
  std::uint64_t document_revision{0};
  std::size_t damage_rects{0};
  std::size_t paint_nodes{0};
  bool full_repaint{false};
};

} // namespace white
