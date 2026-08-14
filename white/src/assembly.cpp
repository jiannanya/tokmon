#include <white/assembly.hpp>

#include <arche/plugin.hpp>

namespace white {
namespace {

class LexborDomService final : public DomService {
public:
  Document parse(std::string_view html,
                 std::string_view css) const override {
    return Document::parse_html(html, css);
  }
};

class DefaultStyleService final : public StyleService {
public:
  void apply(Document& document, std::string_view css) const override {
    document.set_style_sheet(StyleSheet::parse(css));
  }
};

class YogaLayoutService final : public LayoutService {
public:
  void layout(Document& document, float width, float height) const override {
    document.layout(width, height);
  }
};

class SkiaRenderBackend final : public RenderBackend {
public:
  void render(RasterSurface& surface,
              const Document& document) const override {
    surface.render(document);
  }
};

template <typename Service>
class ServicePlugin final : public arche::Plugin {
public:
  ServicePlugin(std::string id, std::string capability,
                std::string interface_hash,
                std::vector<arche::CapabilityRequirement> requirements,
                std::shared_ptr<Service> service)
      : capability_(std::move(capability)), service_(std::move(service)) {
    descriptor_.id = std::move(id);
    descriptor_.version = "1.0.0";
    descriptor_.requirements = std::move(requirements);
    descriptor_.provides.push_back(
        {capability_, "1.0.0", std::move(interface_hash), false});
  }
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context& context) override {
    context.provide<Service>(capability_, "1.0.0", service_);
  }

private:
  arche::PluginDescriptor descriptor_;
  std::string capability_;
  std::shared_ptr<Service> service_;
};

arche::CapabilityRequirement requirement(std::string capability,
                                         std::string interface_hash) {
  arche::CapabilityRequirement result;
  result.capability = std::move(capability);
  result.range = "^1.0";
  result.interface_hash = std::move(interface_hash);
  return result;
}

} // namespace

Assembly::Assembly(arche::Runtime& runtime) : runtime_(runtime) {
  catalog_.add("org.tokmon.white.dom.lexbor", "1.0.0",
               [](const arche::Json&) {
                 return std::make_shared<ServicePlugin<DomService>>(
                     "org.tokmon.white.dom.lexbor", "white.dom",
                     "white-dom-v1",
                     std::vector<arche::CapabilityRequirement>{},
                     std::make_shared<LexborDomService>());
               });
  catalog_.add("org.tokmon.white.style.default", "1.0.0",
               [](const arche::Json&) {
                 return std::make_shared<ServicePlugin<StyleService>>(
                     "org.tokmon.white.style.default", "white.style",
                     "white-style-v1",
                     std::vector{requirement("white.dom", "white-dom-v1")},
                     std::make_shared<DefaultStyleService>());
               });
  catalog_.add("org.tokmon.white.layout.yoga", "1.0.0",
               [](const arche::Json&) {
                 return std::make_shared<ServicePlugin<LayoutService>>(
                     "org.tokmon.white.layout.yoga", "white.layout",
                     "white-layout-v1",
                     std::vector{
                         requirement("white.dom", "white-dom-v1"),
                         requirement("white.style", "white-style-v1")},
                     std::make_shared<YogaLayoutService>());
               });
  catalog_.add("org.tokmon.white.render.skia-raster", "1.0.0",
               [](const arche::Json&) {
                 return std::make_shared<ServicePlugin<RenderBackend>>(
                     "org.tokmon.white.render.skia-raster", "white.render",
                     "white-render-v1",
                     std::vector{requirement("white.layout",
                                             "white-layout-v1")},
                     std::make_shared<SkiaRenderBackend>());
               });
  catalog_.add("org.tokmon.white.platform.sdl3", "1.0.0",
               [](const arche::Json&) {
                 return std::make_shared<ServicePlugin<RuntimeService>>(
                     "org.tokmon.white.platform.sdl3", "white.runtime",
                     "white-runtime-v1",
                     std::vector{requirement("white.render",
                                             "white-render-v1")},
                     std::make_shared<RuntimeService>());
               });
  reconciler_ = std::make_unique<arche::Reconciler>(catalog_);
  arche::DesiredComposition desired;
  desired.id = "org.tokmon.white.default";
  desired.plugins = {
      {"white.dom", "org.tokmon.white.dom.lexbor@1.0.0", "document"},
      {"white.style", "org.tokmon.white.style.default@1.0.0", "document"},
      {"white.layout", "org.tokmon.white.layout.yoga@1.0.0", "document"},
      {"white.render", "org.tokmon.white.render.skia-raster@1.0.0", "window"},
      {"white.runtime", "org.tokmon.white.platform.sdl3@1.0.0", "window"}};
  composition_report_ = reconciler_->apply(runtime_, desired, "white.");
  dom_ = runtime_.root_context()->require<DomService>("white.dom", "^1.0");
  styles_ = runtime_.root_context()->require<StyleService>(
      "white.style", "^1.0");
  layout_ = runtime_.root_context()->require<LayoutService>(
      "white.layout", "^1.0");
  renderer_ = runtime_.root_context()->require<RenderBackend>(
      "white.render", "^1.0");
  service_ = runtime_.root_context()->require<RuntimeService>(
      "white.runtime", "^1.0");
}

Assembly::~Assembly() {
  service_ = {};
  renderer_ = {};
  layout_ = {};
  styles_ = {};
  dom_ = {};
  for (const auto* instance : {"white.runtime", "white.render",
                               "white.layout", "white.style", "white.dom"}) {
    try {
      runtime_.uninstall(instance);
    } catch (...) {
    }
  }
}

RuntimeService& Assembly::service() { return *service_; }

} // namespace white
