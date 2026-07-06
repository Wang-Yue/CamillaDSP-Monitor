// Device capability discovery for CoreAudio.

#ifndef CLIB_BACKEND_CORE_AUDIO_CAPABILITIES_H
#define CLIB_BACKEND_CORE_AUDIO_CAPABILITIES_H

#ifdef __APPLE__

#include "core_audio_device.h"
#include "Config/engine_config_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// MARK: - Discovery

/// Sample rates we report when a device exposes a *range* rather than a
/// discrete list. CoreAudio devices commonly advertise something like
/// 44.1 kHz – 192 kHz; we report only the standard rates that fall
/// inside the range so the UI doesn't need to render thousands of
/// values.
///
/// Public so room-correction tooling can pre-render an FIR per
/// rate, then pick the matching one at engine-config time.
extern const int CORE_AUDIO_STANDARD_RATES[15];
extern const size_t CORE_AUDIO_STANDARD_RATES_COUNT;

// MARK: Device enumeration
//
// Thin wrappers over `CoreAudioDevice` so the UI doesn't need to
// touch HAL types. Anything beyond a name lives in the capability
// descriptor (`describe`) below.

/// Names of all devices visible to the system in the requested
/// direction. Empty when no devices match (no mics connected, no
/// output devices, etc.).
int core_audio_capabilities_available_device_names(bool is_capture, char out_names[][256], int max_names);

/// Name of the system-default device in the requested direction,
/// if one is configured. Useful as the initial value for a picker.
bool core_audio_capabilities_default_device_name(bool is_capture, char* out_name, size_t max_len);

/// Maximum channel count the named device exposes across any of
/// its physical formats. When `name` is `nil` the system default
/// is queried. Returns `0` if the device can't be located.
///
/// Derived from `describe(deviceName:isCapture:)` — no separate HAL
/// query — so the answer matches whatever the capability descriptor
/// reports. Used by the room-correction UI to populate per-channel
/// pickers (e.g. left/right speaker, calibrated mic capsule on a
/// stereo interface).
int core_audio_capabilities_channel_count(const char* device_name, bool is_capture);

/// Build the capability descriptor for a named device. Returns `nil`
/// if the device cannot be located. All low-level HAL plumbing is
/// delegated to `CoreAudioDevice`; this layer only adds the
/// physical-format probe + aggregation that's specific to the UI's
/// `AudioDeviceDescriptor` shape.
audio_device_descriptor_t* core_audio_capabilities_describe(const char* device_name, bool is_capture);

/// Free the audio device descriptor and its internal capability sets.
void core_audio_capabilities_free_descriptor(audio_device_descriptor_t* desc);

#ifdef __cplusplus
}
#endif

#endif // __APPLE__

#endif // CLIB_BACKEND_CORE_AUDIO_CAPABILITIES_H
