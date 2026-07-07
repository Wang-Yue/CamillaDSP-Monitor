#ifndef CLIB_BACKEND_WASAPI_CAPABILITIES_H
#define CLIB_BACKEND_WASAPI_CAPABILITIES_H

#if defined(ENABLE_WASAPI)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Config/engine_config_types.h"
#include "backend_error.h"

/// Enumerate available WASAPI devices and return count.
int wasapi_capabilities_available_device_names(bool is_capture,
                                               char out_names[][256],
                                               int max_names);
/// Get the name of the default WASAPI device.
bool wasapi_capabilities_default_device_name(bool is_capture, char* out_name,
                                             size_t max_len);
/// Generate capabilities descriptor for a specific WASAPI device.
audio_device_descriptor_t* wasapi_capabilities_describe(const char* device_name,
                                                        bool is_capture,
                                                        device_error_t* err);
/// Free descriptor memory.
void wasapi_capabilities_free_descriptor(audio_device_descriptor_t* desc);

#endif  // ENABLE_WASAPI

#endif  // CLIB_BACKEND_WASAPI_CAPABILITIES_H
