#pragma once

#include <white/window.hpp>

#include <arche/composition.hpp>
#include <arche/runtime.hpp>

namespace white {

class DomService {
public:
  virtual ~DomService() = default;
  [[nodiscard]] virtual Document parse(std::string_view html,
                                       std::string_view css = {}) const = 0;
};

class StyleService {
public:
  virtual ~StyleService() = default;
  virtual void apply(Document& document, std::string_view css) const = 0;
};

class LayoutService {
public:
  virtual ~LayoutService() = default;
  virtual void layout(Document& document, float width, float height) const = 0;
};

class RenderBackend {
public:
  virtual ~RenderBackend() = default;
  virtual void render(RasterSurface& surface,
                      const Document& document) const = 0;
};

class RuntimeService final {
public:
  [[nodiscard]] std::unique_ptr<Window> create_window(
      WindowOptions options = {}) const {
    return std::make_unique<Window>(std::move(options));
  }
};

class Assembly final {
public:
  explicit Assembly(arche::Runtime& runtime);
  ~Assembly();

  [[nodiscard]] RuntimeService& service();
  [[nodiscard]] DomService& dom() { return *dom_; }
  [[nodiscard]] StyleService& styles() { return *styles_; }
  [[nodiscard]] LayoutService& layout() { return *layout_; }
  [[nodiscard]] RenderBackend& renderer() { return *renderer_; }
  [[nodiscard]] const arche::CompositionReport& composition_report() const
      noexcept { return composition_report_; }

private:
  arche::Runtime& runtime_;
  arche::PluginCatalog catalog_;
  std::unique_ptr<arche::Reconciler> reconciler_;
  arche::CompositionReport composition_report_;
  arche::CapabilityLease<DomService> dom_;
  arche::CapabilityLease<StyleService> styles_;
  arche::CapabilityLease<LayoutService> layout_;
  arche::CapabilityLease<RenderBackend> renderer_;
  arche::CapabilityLease<RuntimeService> service_;
};

} // namespace white
