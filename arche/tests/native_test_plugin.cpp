#include <arche/native_abi.h>

#include <cstring>

namespace {

arche_string_view_v1 view(const char* value) {
  return {value, std::strlen(value)};
}

int32_t apply(void*, const arche_host_api_v1* host) {
  if (!host || !host->provide_json) return -1;
  return host->provide_json(host->host_context, view("test.native.value"),
                            view("1.0.0"), view("{\"value\":7}"));
}

void quiesce(void*) {}
void destroy(void*) {}

arche_status create(void*, const arche_host_api_v1*,
                    arche_plugin_v1* out_plugin) {
  if (!out_plugin) return -1;
  *out_plugin = {ARCHE_NATIVE_ABI_V1, nullptr, &apply, &quiesce, &destroy};
  return 0;
}

} // namespace

extern "C" ARCHE_EXPORT arche_status arche_plugin_query_v1(
    const arche_host_api_v1*, arche_plugin_descriptor_v1* out_descriptor) {
  if (!out_descriptor) return -1;
  static const char manifest[] =
      "{\"schema\":\"org.tokmon.arche.plugin/v1\","
      "\"id\":\"test.native-plugin\",\"version\":\"1.0.0\","
      "\"abi\":\"arche-native/1\",\"provides\":[{"
      "\"capability\":\"test.native.value\",\"version\":\"1.0.0\","
      "\"interface_hash\":\"native-json-v1\"}]}";
  *out_descriptor = {ARCHE_NATIVE_ABI_V1,
                     static_cast<uint32_t>(sizeof(void*) * 8U),
                     1U,
                     {0, 0, 0},
                     {manifest, sizeof(manifest) - 1U},
                     view("test-x86_64"),
#ifdef NDEBUG
                     view("release"),
#else
                     view("debug"),
#endif
                     view("msvc"),
                     nullptr,
                     &create};
  return 0;
}
