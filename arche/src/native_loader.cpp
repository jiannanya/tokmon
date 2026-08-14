#include <arche/manifest.hpp>
#include <arche/native_abi.h>
#include <arche/native_loader.hpp>

#include <tokmon/common/types.hpp>

#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace arche {
namespace {

class Module final {
public:
  Module(const std::filesystem::path& path, bool physical_unload)
      : physical_unload_(physical_unload) {
#ifdef _WIN32
    handle_ = LoadLibraryW(path.c_str());
    if (!handle_) {
      throw tokmon::Error("arche.native.load",
                          "failed to load library: " + path.string());
    }
#else
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
      throw tokmon::Error("arche.native.load", dlerror());
    }
#endif
  }

  ~Module() {
    if (!physical_unload_) return;
#ifdef _WIN32
    if (handle_) {
      FreeLibrary(handle_);
    }
#else
    if (handle_) {
      dlclose(handle_);
    }
#endif
  }

  void* symbol(const char* name) const {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(handle_, name));
#else
    return dlsym(handle_, name);
#endif
  }

private:
  bool physical_unload_{false};
#ifdef _WIN32
  HMODULE handle_{};
#else
  void* handle_{};
#endif
};

std::string copy(arche_string_view_v1 value) {
  return {value.data, value.size};
}

struct NativeApplyContext {
  Context* context{};
};

int32_t provide_json(void* opaque, arche_string_view_v1 id,
                     arche_string_view_v1 version,
                     arche_string_view_v1 json_value) {
  try {
    auto& state = *static_cast<NativeApplyContext*>(opaque);
    state.context->provide_json(copy(id), copy(version),
                                Json::parse(copy(json_value)));
    return 0;
  } catch (...) {
    return -1;
  }
}

int32_t add_cleanup(void* opaque, arche_string_view_v1 label,
                    arche_cleanup_fn_v1 cleanup, void* user_data) {
  try {
    auto& state = *static_cast<NativeApplyContext*>(opaque);
    state.context->on_unload(copy(label),
                             [cleanup, user_data] { cleanup(user_data); });
    return 0;
  } catch (...) {
    return -1;
  }
}

void log_message(void*, int32_t, arche_string_view_v1) {}

class NativePlugin final : public Plugin {
public:
  NativePlugin(std::shared_ptr<Module> module, arche_plugin_v1 plugin,
               PluginDescriptor descriptor)
      : module_(std::move(module)),
        plugin_(plugin),
        descriptor_(std::move(descriptor)) {}

  ~NativePlugin() override {
    if (plugin_.destroy) {
      plugin_.destroy(plugin_.plugin_context);
    }
  }

  const PluginDescriptor& descriptor() const override { return descriptor_; }

  void apply(Context& context) override {
    NativeApplyContext state{&context};
    arche_host_api_v1 host{
        ARCHE_NATIVE_ABI_V1, &state, &provide_json, &add_cleanup, &log_message};
    if (!plugin_.apply ||
        plugin_.apply(plugin_.plugin_context, &host) != 0) {
      throw tokmon::Error("arche.native.apply",
                          "native plugin apply failed: " + descriptor_.id);
    }
  }

  void quiesce(Context&) override {
    if (plugin_.quiesce) {
      plugin_.quiesce(plugin_.plugin_context);
    }
  }

private:
  std::shared_ptr<Module> module_;
  arche_plugin_v1 plugin_{};
  PluginDescriptor descriptor_;
};

} // namespace

PluginPtr load_native_plugin(const std::filesystem::path& library_path,
                             NativeLoadOptions options) {
  auto module =
      std::make_shared<Module>(library_path, options.physical_unload);
  const auto query =
      reinterpret_cast<arche_plugin_query_v1_fn>(
          module->symbol("arche_plugin_query_v1"));
  if (!query) {
    throw tokmon::Error("arche.native.symbol",
                        "arche_plugin_query_v1 not found");
  }
  arche_host_api_v1 discovery_host{ARCHE_NATIVE_ABI_V1, nullptr, nullptr,
                                    nullptr, &log_message};
  arche_plugin_descriptor_v1 native_descriptor{};
  if (query(&discovery_host, &native_descriptor) != 0 ||
      native_descriptor.abi_version != ARCHE_NATIVE_ABI_V1 ||
      native_descriptor.pointer_width != sizeof(void*) * 8U ||
      native_descriptor.little_endian != 1U || !native_descriptor.create) {
    throw tokmon::Error("arche.native.abi",
                        "native plugin ABI negotiation failed");
  }
  arche_plugin_v1 plugin{};
  if (native_descriptor.create(native_descriptor.package_context,
                               &discovery_host, &plugin) != 0 ||
      plugin.abi_version != ARCHE_NATIVE_ABI_V1) {
    throw tokmon::Error("arche.native.create",
                        "native plugin instance creation failed");
  }
  const auto descriptor = parse_plugin_manifest(
      Json::parse(copy(native_descriptor.descriptor_json)));
  return std::make_shared<NativePlugin>(std::move(module), plugin, descriptor);
}

} // namespace arche
