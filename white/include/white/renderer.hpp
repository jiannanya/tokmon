#pragma once

#include <white/document.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace white {

enum class TextAlign { left, center, right };

struct RichTextSpan {
  std::string text;
  float size{14};
  Color color{32, 33, 36, 255};
  int weight{400};
  bool monospace{false};
  std::optional<Color> background;
};

class RasterSurface final {
public:
  // Drawing and layout use logical dimensions. The four-argument overload
  // keeps those coordinates stable while rasterizing into a denser buffer.
  RasterSurface(int width, int height);
  RasterSurface(int width, int height, int pixel_width, int pixel_height);
  ~RasterSurface();
  RasterSurface(RasterSurface&&) noexcept;
  RasterSurface& operator=(RasterSurface&&) noexcept;
  RasterSurface(const RasterSurface&) = delete;
  RasterSurface& operator=(const RasterSurface&) = delete;

  void resize(int width, int height);
  void resize(int width, int height, int pixel_width, int pixel_height);
  void clear(Color color);
  void fill_rect(const Rect& rect, Color color, float radius = 0);
  void stroke_rect(const Rect& rect, Color color, float width,
                   float radius = 0);
  void line(float x1, float y1, float x2, float y2, Color color,
            float width = 1);
  void fill_circle(float center_x, float center_y, float radius, Color color);
  void text(std::string_view value, float x, float baseline, float size,
            Color color);
  // Shapes UTF-8 text through SkParagraph, including CJK, emoji fallback,
  // bidirectional text, wrapping and ellipsis. Returns the laid-out height.
  float paragraph(std::string_view value, const Rect& bounds, float size,
                  Color color, int weight = 400, float line_height = 1.35F,
                  std::size_t max_lines = 0,
                  TextAlign align = TextAlign::left,
                  bool monospace = false);
  float rich_paragraph(std::span<const RichTextSpan> spans,
                       const Rect& bounds, float line_height = 1.5F,
                       std::size_t max_lines = 0,
                       TextAlign align = TextAlign::left);
  void push_clip(const Rect& rect);
  void pop_clip();
  void render(const Document& document);

  [[nodiscard]] int width() const noexcept;
  [[nodiscard]] int height() const noexcept;
  [[nodiscard]] int pixel_width() const noexcept;
  [[nodiscard]] int pixel_height() const noexcept;
  [[nodiscard]] const void* pixels() const noexcept;
  [[nodiscard]] std::size_t row_bytes() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace white
