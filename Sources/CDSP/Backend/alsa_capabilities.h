#ifndef CLIB_BACKEND_ALSA_CAPABILITIES_H
#define CLIB_BACKEND_ALSA_CAPABILITIES_H

#if defined(ENABLE_ALSA)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Config/engine_config_types.h"

int alsa_capabilities_available_device_names(bool is_capture,
                                             char out_names[][256],
                                             int max_names);
bool alsa_capabilities_default_device_name(bool is_capture, char* out_name,
                                           size_t max_len);
int alsa_capabilities_channel_count(const char* device_name, bool is_capture);
audio_device_descriptor_t* alsa_capabilities_describe(const char* device_name,
                                                      bool is_capture);
void alsa_capabilities_free_descriptor(audio_device_descriptor_t* desc);

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_CAPABILITIES_H
