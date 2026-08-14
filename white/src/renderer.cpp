#include <white/renderer.hpp>

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRRect.h>
#include <include/core/SkSurface.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontStyle.h>
#include <modules/skparagraph/include/FontCollection.h>
#include <modules/skparagraph/include/ParagraphBuilder.h>
#include <modules/skparagraph/include/ParagraphStyle.h>
#include <modules/skparagraph/include/TextStyle.h>
#include <modules/skunicode/include/SkUnicode_icu.h>

#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#elif defined(__APPLE__)
#include <include/ports/SkFontMgr_mac_ct.h>
#else
#include <include/ports/SkFontMgr_fontconfig.h>
#include <include/ports/SkFontScanner_FreeType.h>
#endif

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace white {
namespace {

SkColor color(Color value) {
  return SkColorSetARGB(value.alpha, value.red, value.green, value.blue);
}

sk_sp<skia::textlayout::FontCollection> font_collection() {
  static const auto collection = [] {
    auto value = sk_make_sp<skia::textlayout::FontCollection>();
#ifdef _WIN32
    value->setDefaultFontManager(SkFontMgr_New_DirectWrite());
#elif defined(__APPLE__)
    value->setDefaultFontManager(SkFontMgr_New_CoreText(nullptr));
#else
    value->setDefaultFontManager(
        SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType()));
#endif
    value->enableFontFallback();
    return value;
  }();
  return collection;
}

template <typename Value>
void append_cache_key(std::string& key, const Value& value) {
  static_assert(std::is_trivially_copyable_v<Value>);
  key.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void append_cache_key(std::string& key, std::string_view value) {
  append_cache_key(key, value.size());
  key.append(value);
}

void append_cache_key(std::string& key, Color value) {
  append_cache_key(key, value.red);
  append_cache_key(key, value.green);
  append_cache_key(key, value.blue);
  append_cache_key(key, value.alpha);
}

void draw_shaped_text(SkCanvas& canvas, const Node& node, SkColor text_color) {
  using namespace skia::textlayout;
  const auto& rect = node.layout();
  const auto& style = node.style();
  ParagraphStyle paragraph_style;
  paragraph_style.setTextDirection(
      style.text_direction == white::TextDirection::right_to_left
          ? skia::textlayout::TextDirection::kRtl
          : skia::textlayout::TextDirection::kLtr);
  TextStyle text_style;
  text_style.setColor(text_color);
  text_style.setFontSize(style.font_size);
  text_style.setHeight(style.line_height);
  text_style.setHeightOverride(true);
  text_style.setFontStyle(
      SkFontStyle(style.font_weight, SkFontStyle::kNormal_Width,
                  SkFontStyle::kUpright_Slant));
  text_style.setFontFamilies(
      {SkString("Segoe UI"), SkString("Microsoft YaHei UI"),
       SkString("Noto Sans"), SkString("sans-serif")});
  paragraph_style.setTextStyle(text_style);
  auto builder = ParagraphBuilder::make(
      paragraph_style, font_collection(), SkUnicodes::ICU::Make());
  builder->pushStyle(text_style);
  builder->addText(node.text().data(), node.text().size());
  auto paragraph = builder->Build();
  paragraph->layout(std::max(1.0F, rect.width - style.padding * 2.0F));
  paragraph->paint(&canvas, rect.x + style.padding, rect.y + style.padding);
}

void render_node(SkCanvas& canvas, const Node& node) {
  const auto& rect = node.layout();
  const auto& style = node.style();
  SkPaint paint;
  paint.setAntiAlias(true);
  const auto alpha = static_cast<std::uint8_t>(
      std::clamp(style.opacity, 0.0F, 1.0F) * 255.0F);
  const auto with_opacity = [&](Color value) {
    value.alpha = static_cast<std::uint8_t>(
        static_cast<unsigned>(value.alpha) * alpha / 255U);
    return value;
  };
  if (style.background.alpha > 0) {
    paint.setColor(color(with_opacity(style.background)));
    paint.setStyle(SkPaint::kFill_Style);
    if (style.border_radius > 0) {
      canvas.drawRoundRect(
          SkRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height),
          style.border_radius, style.border_radius, paint);
    } else {
      canvas.drawRect(
          SkRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height), paint);
    }
  }
  if (style.border_width > 0 && style.border_color.alpha > 0) {
    paint.setColor(color(with_opacity(style.border_color)));
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(style.border_width);
    canvas.drawRoundRect(
        SkRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height),
        style.border_radius, style.border_radius, paint);
  }
  if (node.focused()) {
    paint.setColor(SkColorSetARGB(220, 58, 107, 224));
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(2.0F);
    canvas.drawRoundRect(
        SkRect::MakeXYWH(rect.x + 1, rect.y + 1,
                         std::max(0.0F, rect.width - 2),
                         std::max(0.0F, rect.height - 2)),
        std::max(2.0F, style.border_radius),
        std::max(2.0F, style.border_radius), paint);
  }
  canvas.save();
  if (style.overflow != Overflow::visible) {
    canvas.clipRect(SkRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height));
  }
  if (!node.text().empty()) {
    draw_shaped_text(canvas, node, color(with_opacity(style.color)));
  }
  for (const auto& child : node.children()) {
    render_node(canvas, *child);
  }
  if ((style.overflow == Overflow::scroll ||
       style.overflow == Overflow::automatic) &&
      node.content_height() > rect.height) {
    const auto ratio = rect.height / node.content_height();
    const auto thumb_height = std::max(20.0F, rect.height * ratio);
    const auto available = std::max(0.0F, rect.height - thumb_height - 4.0F);
    const auto progress = node.scroll_offset_y() /
                          std::max(1.0F, node.content_height() - rect.height);
    SkPaint scrollbar;
    scrollbar.setAntiAlias(true);
    scrollbar.setColor(SkColorSetARGB(110, 95, 100, 112));
    canvas.drawRoundRect(
        SkRect::MakeXYWH(rect.x + rect.width - 7.0F,
                         rect.y + 2.0F + available * progress,
                         4.0F, thumb_height),
        2.0F, 2.0F, scrollbar);
  }
  canvas.restore();
}

} // namespace

struct RasterSurface::Impl {
  struct ParagraphEntry {
    std::unique_ptr<skia::textlayout::Paragraph> paragraph;
    std::uint64_t last_used{};
  };

  skia::textlayout::Paragraph* find_paragraph(const std::string& key) {
    if (const auto found = paragraph_cache.find(key);
        found != paragraph_cache.end()) {
      found->second.last_used = ++paragraph_clock;
      return found->second.paragraph.get();
    }
    return nullptr;
  }

  skia::textlayout::Paragraph& store_paragraph(
      std::string key,
      std::unique_ptr<skia::textlayout::Paragraph> paragraph) {
    constexpr std::size_t cache_limit = 512;
    if (paragraph_cache.size() >= cache_limit) {
      const auto oldest = std::ranges::min_element(
          paragraph_cache, {}, [](const auto& item) {
            return item.second.last_used;
      });
      if (oldest != paragraph_cache.end()) paragraph_cache.erase(oldest);
    }
    const auto inserted = paragraph_cache
                              .emplace(std::move(key),
                                       ParagraphEntry{std::move(paragraph),
                                                      ++paragraph_clock})
                              .first;
    return *inserted->second.paragraph;
  }

  int width{};
  int height{};
  int pixel_width{};
  int pixel_height{};
  sk_sp<SkSurface> surface;
  std::unordered_map<std::string, ParagraphEntry> paragraph_cache;
  std::uint64_t paragraph_clock{};

  Impl() { paragraph_cache.reserve(512); }
};

RasterSurface::RasterSurface(int width, int height)
    : RasterSurface(width, height, width, height) {}

RasterSurface::RasterSurface(int width, int height, int pixel_width,
                             int pixel_height)
    : impl_(std::make_unique<Impl>()) {
  resize(width, height, pixel_width, pixel_height);
}
RasterSurface::~RasterSurface() = default;
RasterSurface::RasterSurface(RasterSurface&&) noexcept = default;
RasterSurface& RasterSurface::operator=(RasterSurface&&) noexcept = default;

void RasterSurface::resize(int width, int height) {
  resize(width, height, width, height);
}

void RasterSurface::resize(int width, int height, int pixel_width,
                           int pixel_height) {
  impl_->width = std::max(1, width);
  impl_->height = std::max(1, height);
  impl_->pixel_width = std::max(1, pixel_width);
  impl_->pixel_height = std::max(1, pixel_height);
  impl_->surface = SkSurfaces::Raster(
      SkImageInfo::MakeN32Premul(impl_->pixel_width, impl_->pixel_height));
  if (!impl_->surface) {
    throw std::runtime_error("failed to create Skia raster surface");
  }
  impl_->surface->getCanvas()->scale(
      static_cast<float>(impl_->pixel_width) /
          static_cast<float>(impl_->width),
      static_cast<float>(impl_->pixel_height) /
          static_cast<float>(impl_->height));
}

void RasterSurface::clear(Color value) {
  impl_->surface->getCanvas()->clear(color(value));
}

void RasterSurface::fill_rect(const Rect& rect, Color value, float radius) {
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(color(value));
  paint.setStyle(SkPaint::kFill_Style);
  impl_->surface->getCanvas()->drawRoundRect(
      SkRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height), radius,
      radius, paint);
}

void RasterSurface::stroke_rect(const Rect& rect, Color value, float width,
                                float radius) {
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(color(value));
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(width);
  impl_->surface->getCanvas()->drawRoundRect(
      SkRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height), radius,
      radius, paint);
}

void RasterSurface::line(float x1, float y1, float x2, float y2, Color value,
                         float width) {
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(color(value));
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(width);
  impl_->surface->getCanvas()->drawLine(x1, y1, x2, y2, paint);
}

void RasterSurface::fill_circle(float center_x, float center_y, float radius,
                                Color value) {
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(color(value));
  paint.setStyle(SkPaint::kFill_Style);
  impl_->surface->getCanvas()->drawCircle(center_x, center_y, radius, paint);
}

void RasterSurface::text(std::string_view value, float x, float baseline,
                         float size, Color text_color) {
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(color(text_color));
  SkFont font;
  font.setSize(size);
  impl_->surface->getCanvas()->drawString(std::string(value).c_str(), x,
                                          baseline, font, paint);
}

float RasterSurface::paragraph(std::string_view value, const Rect& bounds,
                               float size, Color text_color, int weight,
                               float line_height, std::size_t max_lines,
                               TextAlign align, bool monospace) {
  using namespace skia::textlayout;
  const auto layout_width =
      std::max(1.0F, std::round(bounds.width / 4.0F) * 4.0F);
  std::string cache_key;
  cache_key.reserve(value.size() + 64);
  cache_key.push_back('P');
  append_cache_key(cache_key, value);
  append_cache_key(cache_key, layout_width);
  append_cache_key(cache_key, size);
  append_cache_key(cache_key, text_color);
  append_cache_key(cache_key, weight);
  append_cache_key(cache_key, line_height);
  append_cache_key(cache_key, max_lines);
  append_cache_key(cache_key, align);
  append_cache_key(cache_key, monospace);
  if (auto* cached = impl_->find_paragraph(cache_key)) {
    cached->paint(impl_->surface->getCanvas(), bounds.x, bounds.y);
    return cached->getHeight();
  }
  ParagraphStyle paragraph_style;
  switch (align) {
  case white::TextAlign::center:
    paragraph_style.setTextAlign(skia::textlayout::TextAlign::kCenter);
    break;
  case white::TextAlign::right:
    paragraph_style.setTextAlign(skia::textlayout::TextAlign::kRight);
    break;
  default:
    paragraph_style.setTextAlign(skia::textlayout::TextAlign::kLeft);
    break;
  }
  if (max_lines > 0) {
    paragraph_style.setMaxLines(max_lines);
    paragraph_style.setEllipsis(SkString("…"));
  }
  TextStyle text_style;
  text_style.setColor(color(text_color));
  text_style.setFontSize(size);
  text_style.setHeight(line_height);
  text_style.setHeightOverride(true);
  text_style.setFontStyle(
      SkFontStyle(weight, SkFontStyle::kNormal_Width,
                  SkFontStyle::kUpright_Slant));
  if (monospace) {
    text_style.setFontFamilies(
        {SkString("Cascadia Mono"), SkString("Consolas"),
         SkString("Noto Sans Mono CJK SC"), SkString("monospace")});
  } else {
    text_style.setFontFamilies(
        {SkString("Segoe UI"), SkString("Microsoft YaHei UI"),
         SkString("Noto Sans CJK SC"), SkString("Noto Sans"),
         SkString("sans-serif")});
  }
  paragraph_style.setTextStyle(text_style);
  auto builder = ParagraphBuilder::make(
      paragraph_style, font_collection(), SkUnicodes::ICU::Make());
  builder->pushStyle(text_style);
  builder->addText(value.data(), value.size());
  auto paragraph = builder->Build();
  paragraph->layout(layout_width);
  auto& cached =
      impl_->store_paragraph(std::move(cache_key), std::move(paragraph));
  cached.paint(impl_->surface->getCanvas(), bounds.x, bounds.y);
  return cached.getHeight();
}

float RasterSurface::rich_paragraph(std::span<const RichTextSpan> spans,
                                    const Rect& bounds, float line_height,
                                    std::size_t max_lines, TextAlign align) {
  using namespace skia::textlayout;
  const auto layout_width =
      std::max(1.0F, std::round(bounds.width / 4.0F) * 4.0F);
  std::size_t text_size = 0;
  for (const auto& span : spans) text_size += span.text.size();
  std::string cache_key;
  cache_key.reserve(text_size + spans.size() * 32 + 48);
  cache_key.push_back('R');
  append_cache_key(cache_key, layout_width);
  append_cache_key(cache_key, line_height);
  append_cache_key(cache_key, max_lines);
  append_cache_key(cache_key, align);
  append_cache_key(cache_key, spans.size());
  for (const auto& span : spans) {
    append_cache_key(cache_key, std::string_view(span.text));
    append_cache_key(cache_key, span.size);
    append_cache_key(cache_key, span.color);
    append_cache_key(cache_key, span.weight);
    append_cache_key(cache_key, span.monospace);
    append_cache_key(cache_key, span.background.has_value());
    if (span.background) append_cache_key(cache_key, *span.background);
  }
  if (auto* cached = impl_->find_paragraph(cache_key)) {
    cached->paint(impl_->surface->getCanvas(), bounds.x, bounds.y);
    return cached->getHeight();
  }
  ParagraphStyle paragraph_style;
  switch (align) {
  case white::TextAlign::center:
    paragraph_style.setTextAlign(skia::textlayout::TextAlign::kCenter);
    break;
  case white::TextAlign::right:
    paragraph_style.setTextAlign(skia::textlayout::TextAlign::kRight);
    break;
  default:
    paragraph_style.setTextAlign(skia::textlayout::TextAlign::kLeft);
    break;
  }
  if (max_lines > 0) {
    paragraph_style.setMaxLines(max_lines);
    paragraph_style.setEllipsis(SkString("…"));
  }
  auto builder = ParagraphBuilder::make(
      paragraph_style, font_collection(), SkUnicodes::ICU::Make());
  for (const auto& span : spans) {
    TextStyle style;
    style.setColor(color(span.color));
    style.setFontSize(span.size);
    style.setHeight(line_height);
    style.setHeightOverride(true);
    style.setFontStyle(
        SkFontStyle(span.weight, SkFontStyle::kNormal_Width,
                    SkFontStyle::kUpright_Slant));
    if (span.monospace) {
      style.setFontFamilies(
          {SkString("Cascadia Mono"), SkString("Consolas"),
           SkString("Noto Sans Mono CJK SC"), SkString("monospace")});
    } else {
      style.setFontFamilies(
          {SkString("Segoe UI"), SkString("Microsoft YaHei UI"),
           SkString("Noto Sans CJK SC"), SkString("Noto Sans"),
           SkString("sans-serif")});
    }
    if (span.background) {
      SkPaint background;
      background.setColor(color(*span.background));
      background.setAntiAlias(true);
      style.setBackgroundPaint(std::move(background));
    }
    builder->pushStyle(style);
    builder->addText(span.text.data(), span.text.size());
    builder->pop();
  }
  auto paragraph = builder->Build();
  paragraph->layout(layout_width);
  auto& cached =
      impl_->store_paragraph(std::move(cache_key), std::move(paragraph));
  cached.paint(impl_->surface->getCanvas(), bounds.x, bounds.y);
  return cached.getHeight();
}

void RasterSurface::push_clip(const Rect& rect) {
  auto* canvas = impl_->surface->getCanvas();
  canvas->save();
  canvas->clipRect(
      SkRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height),
      SkClipOp::kIntersect, true);
}

void RasterSurface::pop_clip() { impl_->surface->getCanvas()->restore(); }

void RasterSurface::render(const Document& document) {
  render_node(*impl_->surface->getCanvas(), document.root());
}

int RasterSurface::width() const noexcept { return impl_->width; }
int RasterSurface::height() const noexcept { return impl_->height; }
int RasterSurface::pixel_width() const noexcept { return impl_->pixel_width; }
int RasterSurface::pixel_height() const noexcept {
  return impl_->pixel_height;
}
const void* RasterSurface::pixels() const noexcept {
  SkPixmap pixmap;
  if (!impl_->surface->peekPixels(&pixmap)) return nullptr;
  return pixmap.addr();
}
std::size_t RasterSurface::row_bytes() const noexcept {
  SkPixmap pixmap;
  if (!impl_->surface->peekPixels(&pixmap)) return 0;
  return pixmap.rowBytes();
}

} // namespace white
