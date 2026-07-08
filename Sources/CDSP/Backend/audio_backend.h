// Audio backend protocols.
//
// `ProcessingState` and `ProcessingStopReason` — used by both the
// engine internals and the public actor — live in `Engine/DSPEngine.swift`.

#ifndef CLIB_BACKEND_AUDIO_BACKEND_H
#define CLIB_BACKEND_AUDIO_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Audio/audio_chunk.h"
#include "Config/engine_config_types.h"
#include "backend_error.h"

/**
 * @file audio_backend.h
 * @brief Interfaces and wrappers for audio capture and playback backends.
 *
 * Defines the Virtual Method Tables (vtables) and wrappers for the
 * capture and playback backends used by the CamillaDSP-Monitor engine.
 */

typedef struct capture_backend capture_backend_t;
typedef struct playback_backend playback_backend_t;

/**
 * @struct capture_backend_vtable
 * @brief Virtual table containing function pointers for audio capture
 * operations.
 */
typedef struct {
  /**
   * @brief Open the capture device.
   * @param ctx Pointer to the backend instance context.
   * @param[out] err Pointer to store error details on failure.
   * @return true on success, false on failure.
   */
  bool (*open)(void* ctx, backend_error_t* err);

  /**
   * @brief Read a chunk of audio into the provided buffer.
   * @param ctx Pointer to the backend instance context.
   * @param frames Number of frames to read.
   * @param[out] chunk Pointer to store the read audio chunk.
   * @param[out] err Pointer to store error details on failure.
   * @return true on success, false on end-of-stream or error.
   */
  bool (*read)(void* ctx, size_t frames, audio_chunk_t* chunk,
               backend_error_t* err);

  /**
   * @brief Close the capture device.
   * @param ctx Pointer to the backend instance context.
   */
  void (*close)(void* ctx);

  /**
   * @brief Check for pending sample rate changes on the capture device.
   *
   * Polled by the engine each chunk to detect if a format change occurred.
   *
   * @param ctx Pointer to the backend instance context.
   * @param[out] out_rate Pointer to store the new sample rate if a change is
   * pending.
   * @return true if a change was detected, false otherwise.
   */
  bool (*get_pending_rate_change)(void* ctx, double* out_rate);

  /**
   * @brief Check if the capture device exposes a tunable clock.
   *
   * Used for rate-adjust loops sending pitch corrections instead of resampling
   * ratio nudges.
   *
   * @param ctx Pointer to the backend instance context.
   * @return true if pitch control is supported, false otherwise.
   */
  bool (*is_pitch_control_supported)(void* ctx);

  /**
   * @brief Apply a clock-pitch correction to the capture device.
   * @param ctx Pointer to the backend instance context.
   * @param multiplier The clock rate multiplier (typically close to 1.0).
   */
  void (*set_pitch)(void* ctx, double multiplier);

  /**
   * @brief Wait for new samples to become available.
   * @param ctx Pointer to the backend instance context.
   * @param timeout_ms Maximum time to wait in milliseconds.
   * @return true if data is available, false on timeout or error.
   */
  bool (*wait_for_data)(void* ctx, uint32_t timeout_ms);

  /**
   * @brief Notify the capture backend of the paused state of the processing
   * loop.
   * @param ctx Pointer to the backend instance context.
   * @param paused true if the loop is paused, false otherwise.
   */
  void (*set_is_paused)(void* ctx, bool paused);

  /**
   * @brief Destroy the capture backend context.
   * @param ctx Pointer to the backend instance context to free.
   */
  void (*destroy)(void* ctx);
} capture_backend_vtable_t;

/**
 * @struct capture_backend
 * @brief Wrapper structure holding the capture backend context and its vtable.
 */
struct capture_backend {
  void* ctx;                              /**< Private context pointer */
  const capture_backend_vtable_t* vtable; /**< Virtual method table */
};

/**
 * @struct playback_backend_vtable
 * @brief Virtual table containing function pointers for audio playback
 * operations.
 */
typedef struct {
  /**
   * @brief Open the playback device.
   * @param ctx Pointer to the backend instance context.
   * @param[out] err Pointer to store error details on failure.
   * @return true on success, false on failure.
   */
  bool (*open)(void* ctx, backend_error_t* err);

  /**
   * @brief Write a chunk of audio to the playback device.
   * @param ctx Pointer to the backend instance context.
   * @param chunk Pointer to the audio chunk to write.
   * @param[out] err Pointer to store error details on failure.
   * @return true on success, false on failure.
   */
  bool (*write)(void* ctx, const audio_chunk_t* chunk, backend_error_t* err);

  /**
   * @brief Close the playback device.
   * @param ctx Pointer to the backend instance context.
   */
  void (*close)(void* ctx);

  /**
   * @brief Get the current playback buffer level in samples.
   * @param ctx Pointer to the backend instance context.
   * @return Buffer level in frames.
   */
  size_t (*get_buffer_level)(void* ctx);

  /**
   * @brief Check for pending sample rate changes on the playback device.
   * @param ctx Pointer to the backend instance context.
   * @param[out] out_rate Pointer to store the new sample rate if a change is
   * pending.
   * @return true if a change was detected, false otherwise.
   */
  bool (*get_pending_rate_change)(void* ctx, double* out_rate);

  /**
   * @brief Prefill the playback buffer with silence.
   *
   * Writes the specified number of silence frames before processing starts
   * to align the rate-adjust controller level.
   *
   * @param ctx Pointer to the backend instance context.
   * @param frames Number of silence frames to write.
   * @param[out] err Pointer to store error details on failure.
   * @return true on success, false on failure.
   */
  bool (*prefill_silence)(void* ctx, size_t frames, backend_error_t* err);

  /**
   * @brief Check if playback is paused.
   * @param ctx Pointer to the backend instance context.
   * @return true if paused, false otherwise.
   */
  bool (*get_is_paused)(void* ctx);

  /**
   * @brief Set the playback paused state.
   * @param ctx Pointer to the backend instance context.
   * @param paused true to pause, false to resume.
   */
  void (*set_is_paused)(void* ctx, bool paused);

  /**
   * @brief Check if the playback device supports pitch control.
   * @param ctx Pointer to the backend instance context.
   * @return true if supported, false otherwise.
   */
  bool (*pitch_control_supported)(void* ctx);

  /**
   * @brief Set pitch multiplier.
   * @param ctx Pointer to the backend instance context.
   * @param multiplier The clock multiplier.
   */
  void (*set_pitch)(void* ctx, double multiplier);

  /**
   * @brief Destroy the playback backend context.
   * @param ctx Pointer to the backend instance context to free.
   */
  void (*destroy)(void* ctx);
} playback_backend_vtable_t;

/**
 * @struct playback_backend
 * @brief Wrapper structure holding the playback backend context and its vtable.
 */
struct playback_backend {
  void* ctx;                               /**< Private context pointer */
  const playback_backend_vtable_t* vtable; /**< Virtual method table */
};

typedef struct processing_parameters processing_parameters_t;

// Factory functions

/**
 * @brief Create a capture backend instance based on the configuration.
 *
 * @param config Capture device configuration.
 * @param sample_rate Nominal sample rate in Hz.
 * @param chunk_size Buffer chunk size in frames.
 * @param full_duplex True if the engine is running in full duplex mode.
 * @param params Processing parameters.
 * @param[out] err Pointer to store error details if creation fails.
 * @return A pointer to the created capture_backend_t interface wrapper, or NULL
 * on error.
 */
capture_backend_t* create_capture_backend(const capture_device_config_t* config,
                                          int sample_rate, int chunk_size,
                                          bool full_duplex,
                                          processing_parameters_t* params,
                                          backend_error_t* err);

/**
 * @brief Create a playback backend instance based on the configuration.
 *
 * @param config Playback device configuration.
 * @param sample_rate Nominal sample rate in Hz.
 * @param chunk_size Buffer chunk size in frames.
 * @param full_duplex True if the engine is running in full duplex mode.
 * @param params Processing parameters.
 * @param[out] err Pointer to store error details if creation fails.
 * @return A pointer to the created playback_backend_t interface wrapper, or
 * NULL on error.
 */
playback_backend_t* create_playback_backend(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    bool full_duplex, processing_parameters_t* params, backend_error_t* err);

// CaptureBackend wrapper methods

/**
 * @brief Open the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 * @param[out] err Pointer to store error details on failure.
 * @return true on success, false on failure.
 */
bool capture_backend_open(capture_backend_t* backend, backend_error_t* err);

/**
 * @brief Read from the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 * @param frames Number of frames to read.
 * @param[out] chunk Output audio chunk buffer.
 * @param[out] err Pointer to store error details on failure.
 * @return true on success, false on failure.
 */
bool capture_backend_read(capture_backend_t* backend, size_t frames,
                          audio_chunk_t* chunk, backend_error_t* err);

/**
 * @brief Close the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 */
void capture_backend_close(capture_backend_t* backend);

/**
 * @brief Get pending rate changes from the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 * @param[out] out_rate Pointer to store the new rate.
 * @return true if a rate change was detected.
 */
bool capture_backend_get_pending_rate_change(capture_backend_t* backend,
                                             double* out_rate);

/**
 * @brief Check if the capture device supports pitch control via wrapper.
 * @param backend Pointer to the capture backend.
 * @return true if supported.
 */
bool capture_backend_pitch_control_supported(capture_backend_t* backend);

/**
 * @brief Set the clock pitch correction for the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 * @param multiplier The pitch multiplier.
 */
void capture_backend_set_pitch(capture_backend_t* backend, double multiplier);

/**
 * @brief Wait for data to be available on the capture device via wrapper.
 * @param backend Pointer to the capture backend.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if data is available, false on timeout.
 */
bool capture_backend_wait(capture_backend_t* backend, uint32_t timeout_ms);

/**
 * @brief Notify the capture backend of paused state via wrapper.
 * @param backend Pointer to the capture backend.
 * @param paused true if paused.
 */
void capture_backend_set_is_paused(capture_backend_t* backend, bool paused);

/**
 * @brief Free the capture backend and its context.
 * @param backend Pointer to the capture backend to free.
 */
void capture_backend_free(capture_backend_t* backend);

// PlaybackBackend wrapper methods

/**
 * @brief Open the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @param[out] err Pointer to store error details on failure.
 * @return true on success, false on failure.
 */
bool playback_backend_open(playback_backend_t* backend, backend_error_t* err);

/**
 * @brief Write to the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @param chunk Audio chunk to write.
 * @param[out] err Pointer to store error details on failure.
 * @return true on success, false on failure.
 */
bool playback_backend_write(playback_backend_t* backend,
                            const audio_chunk_t* chunk, backend_error_t* err);

/**
 * @brief Close the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 */
void playback_backend_close(playback_backend_t* backend);

/**
 * @brief Get current playback buffer level via wrapper.
 * @param backend Pointer to the playback backend.
 * @return Buffer level in frames.
 */
size_t playback_backend_get_buffer_level(playback_backend_t* backend);

/**
 * @brief Get pending rate changes from the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @param[out] out_rate Pointer to store the new rate.
 * @return true if a rate change was detected.
 */
bool playback_backend_get_pending_rate_change(playback_backend_t* backend,
                                              double* out_rate);

/**
 * @brief Prefill silence to the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @param frames Number of silence frames to write.
 * @param[out] err Pointer to store error details on failure.
 * @return true on success, false on failure.
 */
bool playback_backend_prefill_silence(playback_backend_t* backend,
                                      size_t frames, backend_error_t* err);

/**
 * @brief Get paused status from the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @return true if paused.
 */
bool playback_backend_get_is_paused(playback_backend_t* backend);

/**
 * @brief Set paused status for the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @param paused true to pause, false to resume.
 */
void playback_backend_set_is_paused(playback_backend_t* backend, bool paused);

/**
 * @brief Check if the playback device supports pitch control via wrapper.
 * @param backend Pointer to the playback backend.
 * @return true if supported.
 */
bool playback_backend_pitch_control_supported(playback_backend_t* backend);

/**
 * @brief Set the clock pitch correction for the playback device via wrapper.
 * @param backend Pointer to the playback backend.
 * @param multiplier The pitch multiplier.
 */
void playback_backend_set_pitch(playback_backend_t* backend, double multiplier);

/**
 * @brief Free the playback backend and its context.
 * @param backend Pointer to the playback backend to free.
 */
void playback_backend_free(playback_backend_t* backend);

#endif  // CLIB_BACKEND_AUDIO_BACKEND_H
