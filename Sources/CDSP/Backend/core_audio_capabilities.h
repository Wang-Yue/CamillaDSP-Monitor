// Device capability discovery for CoreAudio.

#ifndef CLIB_BACKEND_CORE_AUDIO_CAPABILITIES_H
#define CLIB_BACKEND_CORE_AUDIO_CAPABILITIES_H

#if defined(ENABLE_COREAUDIO)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Config/engine_config_types.h"
#include "backend_error.h"
#include "core_audio_device.h"

/**
 * @file core_audio_capabilities.h
 * @brief Device capability discovery for CoreAudio.
 *
 * This header defines functions to query available CoreAudio devices,
 * their names, default devices, channel counts, and detailed capabilities.
 */

/**
 * @brief Array of standard sample rates reported when a device exposes a range.
 *
 * CoreAudio devices commonly advertise a range (e.g., 44.1 kHz - 192 kHz).
 * This array contains standard rates within that range to avoid cluttering the
 * UI. Public so room-correction tooling can pre-render FIR filters per rate.
 */
extern const int CORE_AUDIO_STANDARD_RATES[15];

/**
 * @brief Number of elements in CORE_AUDIO_STANDARD_RATES.
 */
extern const size_t CORE_AUDIO_STANDARD_RATES_COUNT;

/**
 * @brief Get the names of all available devices in the requested direction.
 *
 * Thin wrapper over `CoreAudioDevice`.
 *
 * @param is_capture True for capture devices, false for playback devices.
 * @param out_names 2D array to store the retrieved names.
 * @param max_names Maximum number of names to retrieve (size of out_names).
 * @return The number of device names written to out_names, or negative on
 * error.
 */
int core_audio_capabilities_available_device_names(bool is_capture,
                                                   char out_names[][256],
                                                   int max_names);

/**
 * @brief Get the name of the system-default device in the requested direction.
 *
 * @param is_capture True for capture default, false for playback default.
 * @param out_name Buffer to store the default device name.
 * @param max_len Size of the out_name buffer.
 * @return true if default device was found and name copied, false otherwise.
 */
bool core_audio_capabilities_default_device_name(bool is_capture,
                                                 char* out_name,
                                                 size_t max_len);

/**
 * @brief Get the maximum channel count the named device exposes.
 *
 * Derived from core_audio_capabilities_describe().
 * Used to populate per-channel pickers.
 *
 * @param device_name Name of the device. If NULL, the system default is
 * queried.
 * @param is_capture True for capture device, false for playback device.
 * @return Maximum channel count, or 0 if device not found.
 */
int core_audio_capabilities_channel_count(const char* device_name,
                                          bool is_capture);

/**
 * @brief Build the capability descriptor for a named device.
 *
 * Delegates low-level HAL plumbing to `CoreAudioDevice`.
 *
 * @param device_name Name of the device.
 * @param is_capture True for capture device, false for playback device.
 * @param err Pointer to device error structure to report errors.
 * @return Pointer to the allocated audio_device_descriptor_t, or NULL on
 * error/not found. Must be freed with
 * free_audio_device_descriptor().
 */
audio_device_descriptor_t* core_audio_capabilities_describe(
    const char* device_name, bool is_capture, device_error_t* err);

#endif  // ENABLE_COREAUDIO

#endif  // CLIB_BACKEND_CORE_AUDIO_CAPABILITIES_H
