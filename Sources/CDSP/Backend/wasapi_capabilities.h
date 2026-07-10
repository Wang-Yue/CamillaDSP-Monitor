/**
 * @file wasapi_capabilities.h
 * @brief WASAPI device capabilities enumeration and description.
 */

#ifndef CLIB_BACKEND_WASAPI_CAPABILITIES_H
#define CLIB_BACKEND_WASAPI_CAPABILITIES_H

#if defined(ENABLE_WASAPI)

#include <mmdeviceapi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Config/engine_config_types.h"
#include "backend_error.h"

/**
 * @brief Enumerate available WASAPI devices and return count.
 *
 * @param is_capture True to enumerate capture devices, false for playback
 * devices.
 * @param out_names 2D array to store the names of the devices.
 * @param max_names Maximum number of names to store in out_names.
 * @return The number of devices found, or a negative value on error.
 */
int wasapi_capabilities_available_device_names(bool is_capture,
                                               char out_names[][256],
                                               int max_names);

/**
 * @brief Get the name of the default WASAPI device.
 *
 * @param is_capture True to get default capture device, false for playback.
 * @param out_name Pointer to buffer to store the default device name.
 * @param max_len Maximum length of the out_name buffer.
 * @return true if successful, false otherwise.
 */
bool wasapi_capabilities_default_device_name(bool is_capture, char* out_name,
                                             size_t max_len);

/**
 * @brief Generate capabilities descriptor for a specific WASAPI device.
 *
 * @param device_name Name of the device to describe.
 * @param is_capture True if the device is a capture device, false if playback.
 * @param err Pointer to a device_error_t to receive error details on failure.
 * @return A pointer to the audio_device_descriptor_t, or NULL on failure.
 */
audio_device_descriptor_t* wasapi_capabilities_describe(const char* device_name,
                                                        bool is_capture,
                                                        device_error_t* err);

IMMDevice* wasapi_find_device_by_name(IMMDeviceEnumerator* enumerator,
                                      const char* name, bool is_capture);

#endif  // ENABLE_WASAPI

#endif  // CLIB_BACKEND_WASAPI_CAPABILITIES_H
