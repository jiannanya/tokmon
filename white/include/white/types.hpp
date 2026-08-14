#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace white {

struct Color {
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};
  std::uint8_t alpha{255};

  static Color parse(std::string_view value);
  [[nodiscard]] std::uint32_t argb() const noexcept {
    return (static_cast<std::uint32_t>(alpha) << 24U) |
           (static_cast<std::uint32_t>(red) << 16U) |
           (static_cast<std::uint32_t>(green) << 8U) |
           static_cast<std::uint32_t>(blue);
  }
  friend bool operator==(const Color&, const Color&) = default;
};

struct Rect {
  float x{0};
  float y{0};
  float width{0};
  float height{0};

  [[nodiscard]] bool contains(float point_x, float point_y) const noexcept {
    return point_x >= x && point_y >= y && point_x < x + width &&
           point_y < y + height;
  }
};

enum class FlexDirection { row, column };
enum class Align { start, center, end, stretch };
enum class Overflow { visible, hidden, scroll, automatic };
enum class TextDirection { automatic, left_to_right, right_to_left };

struct Style {
  std::optional<float> width;
  std::optional<float> height;
  float flex_grow{0};
  FlexDirection flex_direction{FlexDirection::column};
  Align align_items{Align::stretch};
  float gap{0};
  float padding{0};
  float margin{0};
  float border_width{0};
  float border_radius{0};
  float font_size{16};
  float line_height{1.35F};
  int font_weight{400};
  float opacity{1};
  TextDirection text_direction{TextDirection::automatic};
  Color color{30, 30, 34, 255};
  Color background{0, 0, 0, 0};
  Color border_color{0, 0, 0, 0};
  Overflow overflow{Overflow::visible};
};

} // namespace white
