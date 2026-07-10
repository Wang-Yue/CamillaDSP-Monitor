#ifndef CLIB_BACKEND_ASIO_CAPABILITIES_H
#define CLIB_BACKEND_ASIO_CAPABILITIES_H

#if defined(ENABLE_ASIO)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Config/engine_config_types.h"
#include "backend_error.h"

/**
 * @file asio_capabilities.h
 * @brief ASIO device capabilities queries.
 *
 * Provides functions to enumerate ASIO devices (drivers) and query their
 * capabilities, such as supported sample rates and channels.
 */

/**
 * @brief Enumerate available ASIO device (driver) names.
 *
 * @param is_capture Unused under typical ASIO (ASIO is bidirectional, but API
 * may request capture or playback).
 * @param[out] out_names Buffer to write device names, where each name can be up
 * to 256 bytes (including null terminator).
 * @param max_names Maximum number of names that can be written to the out_names
 * array.
 * @return The number of devices written, or a negative value on error.
 */
int asio_capabilities_available_device_names(bool is_capture,
                                             char out_names[][256],
                                             int max_names);

/**
 * @brief Get the name of the default ASIO device (driver).
 *
 * @param is_capture True if querying default capture device, false for
 * playback.
 * @param[out] out_name Buffer to store the default device name.
 * @param max_len Maximum capacity of the out_name buffer.
 * @return true if successful, false if no default device is found or buffer is
 * too small.
 */
bool asio_capabilities_default_device_name(bool is_capture, char* out_name,
                                           size_t max_len);

/**
 * @brief Query and describe the capabilities of a specific ASIO device.
 *
 * Probes the ASIO driver to fetch details about supported sample rates,
 * input/output channel counts, and formats.
 *
 * @param device_name The name of the ASIO device (driver).
 * @param is_capture True to query capture capabilities, false for playback
 * capabilities.
 * @param[out] err Pointer to store error details if the probe fails.
 * @return A pointer to a newly allocated audio_device_descriptor_t representing
 * the capabilities, or NULL on failure. The returned descriptor must be freed
 * using free_audio_device_descriptor().
 */
audio_device_descriptor_t* asio_capabilities_describe(const char* device_name,
                                                      bool is_capture,
                                                      device_error_t* err);

#endif  // ENABLE_ASIO

#endif  // CLIB_BACKEND_ASIO_CAPABILITIES_H
