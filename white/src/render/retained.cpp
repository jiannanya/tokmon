#include <white/retained.hpp>

#include <algorithm>

namespace white {
namespace {

bool overlaps_or_touches(const Rect &left, const Rect &right) noexcept {
  return left.x <= right.x + right.width &&
         right.x <= left.x + left.width &&
         left.y <= right.y + right.height &&
         right.y <= left.y + left.height;
}

Rect joined(const Rect &left, const Rect &right) noexcept {
  const auto x = std::min(left.x, right.x);
  const auto y = std::min(left.y, right.y);
  const auto right_edge =
      std::max(left.x + left.width, right.x + right.width);
  const auto bottom_edge =
      std::max(left.y + left.height, right.y + right.height);
  return {x, y, right_edge - x, bottom_edge - y};
}

} // namespace

void DamageRegion::add(Rect rect) {
  if (full_ || rect.width <= 0 || rect.height <= 0) return;
  for (auto &existing : rects_) {
    if (!overlaps_or_touches(existing, rect)) continue;
    existing = joined(existing, rect);
    return;
  }
  rects_.push_back(rect);
  if (rects_.size() <= 24) return;
  const auto combined = bounds();
  rects_.assign(1, combined);
}

void DamageRegion::merge(const DamageRegion &other) {
  if (full_) return;
  if (other.full_) {
    mark_full();
    return;
  }
  for (const auto &rect : other.rects_) add(rect);
}

void DamageRegion::mark_full() noexcept {
  full_ = true;
  rects_.clear();
}

void DamageRegion::clear() noexcept {
  full_ = false;
  rects_.clear();
}

bool DamageRegion::empty() const noexcept {
  return !full_ && rects_.empty();
}

bool DamageRegion::intersects(const Rect &rect) const noexcept {
  if (full_) return true;
  return std::ranges::any_of(rects_, [&](const Rect &damage) {
    return overlaps_or_touches(damage, rect);
  });
}

Rect DamageRegion::bounds() const noexcept {
  if (rects_.empty()) return {};
  auto result = rects_.front();
  for (std::size_t index = 1; index < rects_.size(); ++index)
    result = joined(result, rects_[index]);
  return result;
}

} // namespace white
