/**
 * @file processing_parameters.h
 * @brief Thread-safe shared parameters and telemetry for audio processing.
 *
 * Concurrency model:
 * Every field is backed by lock-free atomics (`atomic_double_t` or `_Atomic
 * bool`) — no mutexes or locks. Target volume, current volume, and mute states
 * are kept for 5 faders (Main, Aux 1-4) as separate inline atomic variables to
 * avoid heap allocation and conform to real-time requirements.
 */

#ifndef CLIB_AUDIO_PROCESSING_PARAMETERS_H
#define CLIB_AUDIO_PROCESSING_PARAMETERS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Audio/double_helpers.h"
#include "Audio/lock_free_ring_buffer.h"

#ifndef FADER_T_DEFINED
#define FADER_T_DEFINED
/**
 * @brief Enum defining the available faders.
 */
typedef enum {
  FADER_MAIN = 0, /**< Main fader. */
  FADER_AUX1 = 1, /**< Auxiliary fader 1. */
  FADER_AUX2 = 2, /**< Auxiliary fader 2. */
  FADER_AUX3 = 3, /**< Auxiliary fader 3. */
  FADER_AUX4 = 4  /**< Auxiliary fader 4. */
} fader_t;
#endif
#define FADER_COUNT 5

/// Default volume (dB) when an engine starts.
#define PROCESSING_PARAMETERS_DEFAULT_VOLUME 0.0
/// Default mute state.
#define PROCESSING_PARAMETERS_DEFAULT_MUTE false

// MARK: - Storage

/**
 * @brief Structure containing processing parameters and telemetry.
 *
 * This structure holds atomic variables for volume, mute state, signal levels,
 * and telemetry data. It is designed to be shared between the UI thread
 * and the audio processing thread.
 */
typedef struct processing_parameters {
  /** Target volume (dB) for fader 0-4. UI thread writes; audio thread reads. */
  atomic_double_t target_volumes[FADER_COUNT];
  /** Target volume set-at timestamp (nanosecond epoch) for fader 0-4. */
  _Atomic uint64_t target_volume_set_at[FADER_COUNT];
  /** Current volume (dB) for fader 0-4. Audio thread updates during ramping. */
  atomic_double_t current_volumes[FADER_COUNT];
  /** Mute state for fader 0-4. UI thread writes; audio thread reads. */
  _Atomic bool muted[FADER_COUNT];

  size_t capture_channels;  /**< Number of capture channels. */
  size_t playback_channels; /**< Number of playback channels. */

  /** Per-channel capture signal peak levels (dB). Array size: capture_channels.
   */
  atomic_double_t* capture_signal_peak;
  /** Per-channel capture signal RMS levels (dB). Array size: capture_channels.
   */
  atomic_double_t* capture_signal_rms;
  /** Per-channel playback signal peak levels (dB). Array size:
   * playback_channels. */
  atomic_double_t* playback_signal_peak;
  /** Per-channel playback signal RMS levels (dB). Array size:
   * playback_channels. */
  atomic_double_t* playback_signal_rms;

  // MARK: - Telemetry
  atomic_double_t rate_adjust;      /**< Current rate adjustment factor. */
  atomic_double_t buffer_level;     /**< Current buffer level. */
  _Atomic uint64_t clipped_samples; /**< Cumulative count of clipped samples. */
  atomic_double_t processing_load;  /**< Audio processing load (0.0 to 1.0). */
  atomic_double_t
      resampler_load; /**< Resampler processing load (0.0 to 1.0). */
} processing_parameters_t;

/**
 * @brief Creates a new processing parameters instance.
 *
 * @param capture_channels Number of capture channels.
 * @param playback_channels Number of playback channels.
 * @return Pointer to the allocated processing_parameters_t structure, or NULL
 * on failure.
 */
processing_parameters_t* processing_parameters_create(size_t capture_channels,
                                                      size_t playback_channels);

/**
 * @brief Frees the processing parameters instance.
 *
 * @param params Pointer to the processing parameters instance to free.
 */
void processing_parameters_free(processing_parameters_t* params);

/**
 * @brief Gets the target volume for a specific fader.
 *
 * @param params Pointer to the processing parameters.
 * @param fader The fader to query.
 * @return The target volume in dB.
 */
double processing_parameters_get_target_volume_for_fader(
    const processing_parameters_t* params, fader_t fader);

/**
 * @brief Sets the target volume for a specific fader.
 *
 * @param params Pointer to the processing parameters.
 * @param value The target volume in dB.
 * @param fader The fader to set.
 */
void processing_parameters_set_target_volume_for_fader(
    processing_parameters_t* params, double value, fader_t fader);

/**
 * @brief Gets the timestamp when the target volume was last set for a fader.
 *
 * @param params Pointer to the processing parameters.
 * @param fader The fader to query.
 * @return Timestamp in nanoseconds (epoch).
 */
uint64_t processing_parameters_get_target_volume_set_at_for_fader(
    const processing_parameters_t* params, fader_t fader);

/**
 * @brief Gets the current volume for a specific fader.
 *
 * @param params Pointer to the processing parameters.
 * @param fader The fader to query.
 * @return The current volume in dB.
 */
double processing_parameters_get_current_volume_for_fader(
    const processing_parameters_t* params, fader_t fader);

/**
 * @brief Sets the current volume for a specific fader.
 *
 * @param params Pointer to the processing parameters.
 * @param value The current volume in dB.
 * @param fader The fader to set.
 */
void processing_parameters_set_current_volume_for_fader(
    processing_parameters_t* params, double value, fader_t fader);

/**
 * @brief Checks if a specific fader is muted.
 *
 * @param params Pointer to the processing parameters.
 * @param fader The fader to query.
 * @return True if muted, false otherwise.
 */
bool processing_parameters_is_muted_for_fader(
    const processing_parameters_t* params, fader_t fader);

/**
 * @brief Sets the mute state for a specific fader.
 *
 * @param params Pointer to the processing parameters.
 * @param value The mute state to set (true for muted, false for unmuted).
 * @param fader The fader to set.
 */
void processing_parameters_set_muted_for_fader(processing_parameters_t* params,
                                               bool value, fader_t fader);

/**
 * @brief Gets the target volume for the main fader.
 *
 * @param params Pointer to the processing parameters.
 * @return The target volume in dB.
 */
static inline double processing_parameters_get_target_volume(
    const processing_parameters_t* params) {
  return processing_parameters_get_target_volume_for_fader(params, FADER_MAIN);
}

/**
 * @brief Sets the target volume for the main fader.
 *
 * @param params Pointer to the processing parameters.
 * @param value The target volume in dB.
 */
static inline void processing_parameters_set_target_volume(
    processing_parameters_t* params, double value) {
  processing_parameters_set_target_volume_for_fader(params, value, FADER_MAIN);
}

/**
 * @brief Gets the current volume for the main fader.
 *
 * @param params Pointer to the processing parameters.
 * @return The current volume in dB.
 */
static inline double processing_parameters_get_current_volume(
    const processing_parameters_t* params) {
  return processing_parameters_get_current_volume_for_fader(params, FADER_MAIN);
}

/**
 * @brief Sets the current volume for the main fader.
 *
 * @param params Pointer to the processing parameters.
 * @param value The current volume in dB.
 */
static inline void processing_parameters_set_current_volume(
    processing_parameters_t* params, double value) {
  processing_parameters_set_current_volume_for_fader(params, value, FADER_MAIN);
}

/**
 * @brief Checks if the main fader is muted.
 *
 * @param params Pointer to the processing parameters.
 * @return True if muted, false otherwise.
 */
static inline bool processing_parameters_is_muted(
    const processing_parameters_t* params) {
  return processing_parameters_is_muted_for_fader(params, FADER_MAIN);
}

/**
 * @brief Sets the mute state for the main fader.
 *
 * @param params Pointer to the processing parameters.
 * @param value The mute state to set.
 */
static inline void processing_parameters_set_muted(
    processing_parameters_t* params, bool value) {
  processing_parameters_set_muted_for_fader(params, value, FADER_MAIN);
}

/**
 * @brief Gets the capture signal peak levels.
 *
 * @param params Pointer to the processing parameters.
 * @param out_levels Array to store the peak levels.
 * @param count Number of channels to query. Must be <= capture_channels.
 */
void processing_parameters_get_capture_signal_peak(
    const processing_parameters_t* params, double* out_levels, size_t count);

/**
 * @brief Sets the capture signal peak levels.
 *
 * @param params Pointer to the processing parameters.
 * @param levels Array containing the peak levels.
 * @param count Number of channels to set. Must be <= capture_channels.
 */
void processing_parameters_set_capture_signal_peak(
    processing_parameters_t* params, const double* levels, size_t count);

/**
 * @brief Gets the capture signal RMS levels.
 *
 * @param params Pointer to the processing parameters.
 * @param out_levels Array to store the RMS levels.
 * @param count Number of channels to query. Must be <= capture_channels.
 */
void processing_parameters_get_capture_signal_rms(
    const processing_parameters_t* params, double* out_levels, size_t count);

/**
 * @brief Sets the capture signal RMS levels.
 *
 * @param params Pointer to the processing parameters.
 * @param levels Array containing the RMS levels.
 * @param count Number of channels to set. Must be <= capture_channels.
 */
void processing_parameters_set_capture_signal_rms(
    processing_parameters_t* params, const double* levels, size_t count);

/**
 * @brief Gets the playback signal peak levels.
 *
 * @param params Pointer to the processing parameters.
 * @param out_levels Array to store the peak levels.
 * @param count Number of channels to query. Must be <= playback_channels.
 */
void processing_parameters_get_playback_signal_peak(
    const processing_parameters_t* params, double* out_levels, size_t count);

/**
 * @brief Sets the playback signal peak levels.
 *
 * @param params Pointer to the processing parameters.
 * @param levels Array containing the peak levels.
 * @param count Number of channels to set. Must be <= playback_channels.
 */
void processing_parameters_set_playback_signal_peak(
    processing_parameters_t* params, const double* levels, size_t count);

/**
 * @brief Gets the playback signal RMS levels.
 *
 * @param params Pointer to the processing parameters.
 * @param out_levels Array to store the RMS levels.
 * @param count Number of channels to query. Must be <= playback_channels.
 */
void processing_parameters_get_playback_signal_rms(
    const processing_parameters_t* params, double* out_levels, size_t count);

/**
 * @brief Sets the playback signal RMS levels.
 *
 * @param params Pointer to the processing parameters.
 * @param levels Array containing the RMS levels.
 * @param count Number of channels to set. Must be <= playback_channels.
 */
void processing_parameters_set_playback_signal_rms(
    processing_parameters_t* params, const double* levels, size_t count);

// MARK: - Chunk-based updates (no-allocation, audio-thread safe)

/**
 * @brief Asynchronously update the capture-side peak and RMS levels on the
 * audio thread.
 *
 * This function calculates peak and RMS levels from the provided chunk and
 * updates the corresponding atomic fields in `params`. It is lock-free and does
 * not allocate memory.
 *
 * @param params Pointer to the processing parameters.
 * @param chunk Pointer to the audio chunk.
 * @return The peak level of the chunk (max across all channels in the chunk).
 */
double processing_parameters_update_capture_levels(
    processing_parameters_t* params, const audio_chunk_t* chunk);

/**
 * @brief Asynchronously update the playback-side peak and RMS levels on the
 * audio thread.
 *
 * This function calculates peak and RMS levels from the provided chunk and
 * updates the corresponding atomic fields in `params`. It is lock-free and does
 * not allocate memory.
 *
 * @param params Pointer to the processing parameters.
 * @param chunk Pointer to the audio chunk.
 * @return The peak level of the chunk (max across all channels in the chunk).
 */
double processing_parameters_update_playback_levels(
    processing_parameters_t* params, const audio_chunk_t* chunk);

#endif  // CLIB_AUDIO_PROCESSING_PARAMETERS_H
