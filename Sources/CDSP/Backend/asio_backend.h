#ifndef CLIB_BACKEND_ASIO_BACKEND_H
#define CLIB_BACKEND_ASIO_BACKEND_H

#if defined(ENABLE_ASIO)

#include "audio_backend.h"

/**
 * @file asio_backend.h
 * @brief ASIO capture and playback backend factory.
 *
 * Provides factory functions to create capture and playback backend instances
 * using the ASIO (Audio Stream Input/Output) API.
 */

/**
 * @struct asio_capture
 * @brief Opaque structure representing the ASIO capture backend.
 */
typedef struct asio_capture asio_capture_t;

/**
 * @struct asio_playback
 * @brief Opaque structure representing the ASIO playback backend.
 */
typedef struct asio_playback asio_playback_t;

/**
 * @brief Create a new ASIO capture backend instance.
 *
 * @param config Capture device configuration.
 * @param sample_rate Nominal sample rate in Hz.
 * @param chunk_size Buffer chunk size in frames.
 * @param full_duplex True if the engine is running in full duplex mode.
 * @param[out] err Pointer to store error details if creation fails.
 * @return A pointer to the capture_backend_t interface wrapper, or NULL on
 * error.
 */
capture_backend_t* asio_capture_new(const capture_device_config_t* config,
                                    int sample_rate, int chunk_size,
                                    bool full_duplex, backend_error_t* err);

/**
 * @brief Create a new ASIO playback backend instance.
 *
 * @param config Playback device configuration.
 * @param sample_rate Nominal sample rate in Hz.
 * @param chunk_size Buffer chunk size in frames.
 * @param full_duplex True if the engine is running in full duplex mode.
 * @param[out] err Pointer to store error details if creation fails.
 * @return A pointer to the playback_backend_t interface wrapper, or NULL on
 * error.
 */
playback_backend_t* asio_playback_new(const playback_device_config_t* config,
                                      int sample_rate, int chunk_size,
                                      bool full_duplex, backend_error_t* err);

#endif  // ENABLE_ASIO

#endif  // CLIB_BACKEND_ASIO_BACKEND_H
