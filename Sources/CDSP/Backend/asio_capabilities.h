#ifndef CLIB_BACKEND_ASIO_CAPABILITIES_H
#define CLIB_BACKEND_ASIO_CAPABILITIES_H

#if defined(_WIN32)

#include "Config/engine_config_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Enumerate available ASIO drivers and return count.
int asio_capabilities_available_device_names(bool is_capture, char out_names[][256], int max_names);
/// Get the name of the default ASIO driver.
bool asio_capabilities_default_device_name(bool is_capture, char* out_name, size_t max_len);
/// Generate capabilities descriptor for a specific ASIO driver.
audio_device_descriptor_t* asio_capabilities_describe(const char* device_name, bool is_capture);
/// Free descriptor memory.
void asio_capabilities_free_descriptor(audio_device_descriptor_t* desc);

#ifdef __cplusplus
}
#endif

#endif // _WIN32

#endif // CLIB_BACKEND_ASIO_CAPABILITIES_H
