#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace white {

class VirtualList final {
public:
  void configure(std::size_t item_count, float item_extent,
                 float viewport_extent, std::size_t overscan = 3) noexcept {
    count_ = item_count;
    extent_ = std::max(1.0F, item_extent);
    viewport_ = std::max(0.0F, viewport_extent);
    overscan_ = overscan;
    set_scroll_offset(offset_);
  }

  void set_scroll_offset(float value) noexcept {
    offset_ = std::clamp(value, 0.0F, max_scroll_offset());
  }
  void scroll_by(float delta) noexcept { set_scroll_offset(offset_ + delta); }
  void scroll_to_end() noexcept { offset_ = max_scroll_offset(); }

  [[nodiscard]] float scroll_offset() const noexcept { return offset_; }
  [[nodiscard]] float content_extent() const noexcept {
    return static_cast<float>(count_) * extent_;
  }
  [[nodiscard]] float max_scroll_offset() const noexcept {
    return std::max(0.0F, content_extent() - viewport_);
  }
  [[nodiscard]] std::pair<std::size_t, std::size_t> visible_range() const noexcept {
    if (count_ == 0) return {0, 0};
    const auto first_visible = static_cast<std::size_t>(
        std::floor(offset_ / extent_));
    const auto visible_count = static_cast<std::size_t>(
        std::ceil(viewport_ / extent_)) + 1;
    const auto first = first_visible > overscan_ ? first_visible - overscan_ : 0;
    const auto end = std::min(count_, first_visible + visible_count + overscan_);
    return {first, end};
  }
  [[nodiscard]] float item_offset(std::size_t index) const noexcept {
    return static_cast<float>(index) * extent_ - offset_;
  }

private:
  std::size_t count_{0};
  std::size_t overscan_{3};
  float extent_{1};
  float viewport_{0};
  float offset_{0};
};

} // namespace white
