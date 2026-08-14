#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define ARCHE_EXPORT __declspec(dllexport)
#else
#define ARCHE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHE_NATIVE_ABI_V1 1u

typedef int32_t arche_status;

typedef struct arche_string_view_v1 {
  const char* data;
  size_t size;
} arche_string_view_v1;

typedef void (*arche_cleanup_fn_v1)(void* user_data);

typedef struct arche_host_api_v1 {
  uint32_t abi_version;
  void* host_context;
  int32_t (*provide_json)(void* host_context, arche_string_view_v1 id,
                          arche_string_view_v1 version,
                          arche_string_view_v1 json_value);
  int32_t (*add_cleanup)(void* host_context, arche_string_view_v1 label,
                         arche_cleanup_fn_v1 cleanup, void* user_data);
  void (*log)(void* host_context, int32_t level,
              arche_string_view_v1 message);
} arche_host_api_v1;

typedef struct arche_plugin_v1 {
  uint32_t abi_version;
  void* plugin_context;
  int32_t (*apply)(void* plugin_context, const arche_host_api_v1* host);
  void (*quiesce)(void* plugin_context);
  void (*destroy)(void* plugin_context);
} arche_plugin_v1;

typedef struct arche_plugin_descriptor_v1 {
  uint32_t abi_version;
  uint32_t pointer_width;
  uint8_t little_endian;
  uint8_t reserved[3];
  arche_string_view_v1 descriptor_json;
  arche_string_view_v1 target_triple;
  arche_string_view_v1 build_mode;
  arche_string_view_v1 toolchain_id;
  void* package_context;
  arche_status (*create)(void* package_context,
                         const arche_host_api_v1* host,
                         arche_plugin_v1* out_plugin);
} arche_plugin_descriptor_v1;

typedef arche_status (*arche_plugin_query_v1_fn)(
    const arche_host_api_v1* host,
    arche_plugin_descriptor_v1* out_descriptor);

#ifdef __cplusplus
}
#endif
