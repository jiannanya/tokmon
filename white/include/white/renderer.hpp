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
enum class SurfaceBackend { cpu, gpu };

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
  // The GPU overload binds a Skia Ganesh surface to the current OpenGL
  // framebuffer. A current GL context must exist for the surface lifetime.
  RasterSurface(int width, int height, int pixel_width, int pixel_height,
                SurfaceBackend backend, int sample_count = 0,
                int stencil_bits = 8);
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
  void render(Document& document);
  [[nodiscard]] std::uint64_t
  document_revision(const Document& document) const noexcept;
  [[nodiscard]] FrameMetrics frame_metrics() const noexcept;
  // Window hosts call begin_frame before product drawing. If a retained
  // document participates, frame_damage contains the exact presentation
  // region; an empty region means the host must conservatively present all.
  void begin_frame() noexcept;
  [[nodiscard]] DamageRegion frame_damage() const;
  // Submits pending Ganesh commands. It is a no-op for raster surfaces.
  void flush();
  [[nodiscard]] SurfaceBackend backend() const noexcept;

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
