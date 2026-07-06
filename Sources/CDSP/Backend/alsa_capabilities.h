#ifndef CLIB_BACKEND_ALSA_CAPABILITIES_H
#define CLIB_BACKEND_ALSA_CAPABILITIES_H

#ifndef __APPLE__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Config/engine_config_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int alsa_capabilities_available_device_names(bool is_capture,
                                             char out_names[][256],
                                             int max_names);
bool alsa_capabilities_default_device_name(bool is_capture, char* out_name,
                                           size_t max_len);
int alsa_capabilities_channel_count(const char* device_name, bool is_capture);
audio_device_descriptor_t* alsa_capabilities_describe(const char* device_name,
                                                      bool is_capture);
void alsa_capabilities_free_descriptor(audio_device_descriptor_t* desc);

#ifdef __cplusplus
}
#endif

#endif  // !__APPLE__

#endif  // CLIB_BACKEND_ALSA_CAPABILITIES_H
