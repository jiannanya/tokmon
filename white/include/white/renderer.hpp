#pragma once

#include <white/document.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace white {

class RasterSurface final {
public:
  RasterSurface(int width, int height);
  ~RasterSurface();
  RasterSurface(RasterSurface&&) noexcept;
  RasterSurface& operator=(RasterSurface&&) noexcept;
  RasterSurface(const RasterSurface&) = delete;
  RasterSurface& operator=(const RasterSurface&) = delete;

  void resize(int width, int height);
  void clear(Color color);
  void fill_rect(const Rect& rect, Color color, float radius = 0);
  void stroke_rect(const Rect& rect, Color color, float width,
                   float radius = 0);
  void text(std::string_view value, float x, float baseline, float size,
            Color color);
  void render(const Document& document);

  [[nodiscard]] int width() const noexcept;
  [[nodiscard]] int height() const noexcept;
  [[nodiscard]] const void* pixels() const noexcept;
  [[nodiscard]] std::size_t row_bytes() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace white

