#ifndef TOKMON_SNOW_C_API_H
#define TOKMON_SNOW_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNOW_C_ABI_VERSION_V1 1u

typedef struct snow_host_v1 snow_host_v1;

typedef struct snow_buffer_v1 {
  char* data;
  size_t size;
} snow_buffer_v1;

typedef enum snow_status_v1 {
  SNOW_STATUS_OK_V1 = 0,
  SNOW_STATUS_INVALID_ARGUMENT_V1 = 1,
  SNOW_STATUS_ERROR_V1 = 2
} snow_status_v1;

// Creates a complete Snow/Arche host. bootstrap_json must use schema
// org.tokmon.snow.bootstrap/v1 and contain workspace. data_root,
// config_dir_name and raw_trace are optional.
uint32_t snow_abi_version_v1(void);
snow_status_v1 snow_host_create_v1(const char* bootstrap_json,
                                   size_t bootstrap_size,
                                   snow_host_v1** out_host);
void snow_host_destroy_v1(snow_host_v1* host);

// Invokes one Snow Protocol JSON-RPC request. This is thread-safe; a caller
// may run turn.start on one thread and turn.cancel/turn.steer on another.
snow_status_v1 snow_host_invoke_v1(snow_host_v1* host,
                                   const char* request_json,
                                   size_t request_size,
                                   snow_buffer_v1* out_response);

void snow_buffer_release_v1(snow_buffer_v1* buffer);
const char* snow_last_error_code_v1(void);
const char* snow_last_error_message_v1(void);

#ifdef __cplusplus
}
#endif

#endif
