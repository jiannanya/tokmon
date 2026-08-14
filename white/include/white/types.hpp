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
  [[nodiscard]] bool empty() const noexcept {
    return width <= 0 || height <= 0;
  }
  friend bool operator==(const Rect&, const Rect&) = default;
};

enum class FlexDirection { row, column };
enum class Align { start, center, end, stretch };
enum class Justify { start, center, end, space_between, space_around };
enum class Position { relative, absolute };
enum class Overflow { visible, hidden, scroll, automatic };
enum class TextDirection { automatic, left_to_right, right_to_left };

struct Style {
  std::optional<float> width;
  std::optional<float> height;
  std::optional<float> min_width;
  std::optional<float> min_height;
  std::optional<float> max_width;
  std::optional<float> max_height;
  std::optional<float> left;
  std::optional<float> top;
  std::optional<float> right;
  std::optional<float> bottom;
  float flex_grow{0};
  float flex_shrink{0};
  FlexDirection flex_direction{FlexDirection::column};
  Align align_items{Align::stretch};
  std::optional<Align> align_self;
  Justify justify_content{Justify::start};
  Position position{Position::relative};
  float gap{0};
  float padding{0};
  std::optional<float> padding_left;
  std::optional<float> padding_top;
  std::optional<float> padding_right;
  std::optional<float> padding_bottom;
  float margin{0};
  std::optional<float> margin_left;
  std::optional<float> margin_top;
  std::optional<float> margin_right;
  std::optional<float> margin_bottom;
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

  friend bool operator==(const Style&, const Style&) = default;
};

} // namespace white
