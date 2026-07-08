// Shared CoreAudio HAL helpers used by both `CoreAudioBackend` (the
// capture/playback runtime) and `CoreAudioCapabilities` (the device
// description discovery). Keeps the boilerplate around
// `AudioObjectGetPropertyData` and friends in one place so the two
// backends don't carry near-identical copies of every enumeration helper.

#ifndef CLIB_BACKEND_CORE_AUDIO_DEVICE_H
#define CLIB_BACKEND_CORE_AUDIO_DEVICE_H

#if defined(ENABLE_COREAUDIO)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file core_audio_device.h
 * @brief Shared CoreAudio HAL helpers.
 *
 * This header defines helper functions and structures for interacting with the
 * CoreAudio Hardware Abstraction Layer (HAL). It includes device enumeration,
 * property queries (name, streams, sample rate, buffer size), and clock/pitch
 * control.
 */

/**
 * @brief Direction marker for HAL device queries.
 *
 * Maps to CoreAudio scopes. Input scope is for capture, Output scope is for
 * playback.
 */
typedef enum {
  CORE_AUDIO_SCOPE_INPUT = 0, /**< Input scope (capture). */
  CORE_AUDIO_SCOPE_OUTPUT = 1 /**< Output scope (playback). */
} core_audio_scope_t;

/**
 * @brief Structure containing basic information about a CoreAudio device.
 */
typedef struct {
  AudioDeviceID id; /**< CoreAudio Device ID. */
  char name[256];   /**< User-facing name of the device. */
} core_audio_device_info_t;

// MARK: - Enumeration

/**
 * @brief Retrieve all HAL device IDs on the system.
 *
 * @param out_ids Array to store the retrieved Device IDs.
 * @param max_ids Maximum number of IDs to retrieve (size of out_ids).
 * @return Number of Device IDs written, or negative on error.
 */
int core_audio_device_all_ids(AudioDeviceID* out_ids, int max_ids);

/**
 * @brief Retrieve the user-facing name of a device.
 *
 * @param device_id The HAL Device ID.
 * @param out_name Buffer to store the device name.
 * @param max_len Size of the out_name buffer.
 * @return true if successful and name copied, false otherwise.
 */
bool core_audio_device_name(AudioDeviceID device_id, char* out_name,
                            size_t max_len);

/**
 * @brief Check if the device exposes any streams in the given direction.
 *
 * @param device_id The HAL Device ID.
 * @param scope The direction scope (input/output).
 * @return true if streams exist, false otherwise.
 */
bool core_audio_device_has_stream(AudioDeviceID device_id,
                                  core_audio_scope_t scope);

/**
 * @brief Retrieve HAL stream IDs for the given device and direction.
 *
 * @param device_id The HAL Device ID.
 * @param scope The direction scope.
 * @param out_streams Array to store the retrieved Stream IDs.
 * @param max_streams Maximum number of streams to retrieve.
 * @return Number of Stream IDs written, or negative on error.
 */
int core_audio_device_streams(AudioDeviceID device_id, core_audio_scope_t scope,
                              AudioStreamID* out_streams, int max_streams);

/**
 * @brief List all devices that have at least one stream in the requested
 * direction.
 *
 * @param scope The direction scope.
 * @param out_devices Array of core_audio_device_info_t to store results.
 * @param max_devices Maximum number of devices to retrieve.
 * @return Number of devices written to out_devices, or negative on error.
 */
int core_audio_device_list_devices(core_audio_scope_t scope,
                                   core_audio_device_info_t* out_devices,
                                   int max_devices);

// MARK: - Lookup

/**
 * @brief Get the HAL Device ID of the system-default device for the given
 * direction.
 *
 * @param scope The direction scope.
 * @return The default Device ID, or kAudioObjectUnknown on error.
 */
AudioDeviceID core_audio_device_default_id(core_audio_scope_t scope);

/**
 * @brief Get the HAL Device ID of a device with the specified name.
 *
 * @param name The name of the device. If NULL, returns the default device ID.
 * @param scope The direction scope.
 * @return The Device ID, or kAudioObjectUnknown if not found.
 */
AudioDeviceID core_audio_device_id_for_name(const char* name,
                                            core_audio_scope_t scope);

// MARK: - Sample-rate control

/**
 * @brief Set the nominal sample rate of a device and wait for it to apply.
 *
 * CoreAudio applies rate changes asynchronously. This function blocks/polls
 * until the change is committed to prevent AudioUnits from latching onto old
 * rates.
 *
 * @param device_id The HAL Device ID.
 * @param rate The target sample rate in Hz.
 * @return true if the rate was successfully set and verified, false otherwise.
 */
bool core_audio_device_set_nominal_sample_rate(AudioDeviceID device_id,
                                               double rate);

/**
 * @brief Read the current nominal sample rate of a device.
 *
 * @param device_id The HAL Device ID.
 * @param out_rate Pointer to double to store the retrieved rate.
 * @return true if successful, false otherwise.
 */
bool core_audio_device_get_nominal_sample_rate(AudioDeviceID device_id,
                                               double* out_rate);

// MARK: - Buffer frame size control

/**
 * @brief Set the device's buffer frame size for a given scope.
 *
 * @param device_id The HAL Device ID.
 * @param frames The target buffer size in frames.
 * @param scope The direction scope.
 * @return true on success, false otherwise.
 */
bool core_audio_device_set_buffer_frame_size(AudioDeviceID device_id,
                                             uint32_t frames,
                                             core_audio_scope_t scope);

/**
 * @brief Read the device's current buffer frame size for a given scope.
 *
 * @param device_id The HAL Device ID.
 * @param scope The direction scope.
 * @param out_frames Pointer to uint32_t to store the retrieved buffer size.
 * @return true on success, false otherwise.
 */
bool core_audio_device_get_buffer_frame_size(AudioDeviceID device_id,
                                             core_audio_scope_t scope,
                                             uint32_t* out_frames);

// MARK: - Clock-source / pitch control (BlackHole 0.5.0+)

/**
 * @brief Set the device's active clock source by ID.
 *
 * @param device_id The HAL Device ID.
 * @param source_id The clock source ID to set.
 * @return true on success, false otherwise.
 */
bool core_audio_device_set_clock_source_id(AudioDeviceID device_id,
                                           uint32_t source_id);

/**
 * @brief Select the "Internal Adjustable" clock source if available.
 *
 * Specifically for virtual devices like BlackHole 0.5.0+ to enable pitch
 * tuning.
 *
 * @param device_id The HAL Device ID.
 * @return true if adjustable clock source was selected, false if not
 * supported/failed.
 */
bool core_audio_device_select_adjustable_clock_source(AudioDeviceID device_id);

/**
 * @brief Apply a clock-pitch correction to the device.
 *
 * Uses kAudioDevicePropertyStereoPan to adjust pitch (typically for BlackHole).
 * Maps pitch in [0.99, 1.01] to pan in [0, 1] using formula:
 * pan = (pitch - 1.0) * 50.0 + 0.5 (clamped).
 *
 * @param device_id The HAL Device ID.
 * @param pitch The pitch multiplier.
 */
void core_audio_device_set_pitch(AudioDeviceID device_id, double pitch);

/**
 * @brief Check if the device exposes the nominal-sample-rate property.
 *
 * Useful to check before registering listeners.
 *
 * @param device_id The HAL Device ID.
 * @return true if property exists, false otherwise.
 */
bool core_audio_device_has_nominal_sample_rate_property(
    AudioDeviceID device_id);

// MARK: - Stream-format builder

/**
 * @brief Helper to build a standard 32-bit linear-PCM
 * AudioStreamBasicDescription (ASBD).
 *
 * @param sample_rate Sample rate in Hz.
 * @param channels Number of channels.
 * @param interleaved True for interleaved layout, false for non-interleaved
 * (preferred).
 * @return The constructed ASBD.
 */
AudioStreamBasicDescription core_audio_device_float32_stream_format(
    double sample_rate, int channels, bool interleaved);

/**
 * @brief Set physical format of a device matching sample rate, format string,
 * and channels.
 *
 * @param device_id The HAL Device ID.
 * @param scope The direction scope.
 * @param sample_rate Target sample rate.
 * @param format_str Format description string (e.g. "FLOAT32").
 * @param requested_channels Requested channel count.
 * @return true on success, false otherwise.
 */
bool core_audio_device_set_matching_physical_format(AudioDeviceID device_id,
                                                    core_audio_scope_t scope,
                                                    double sample_rate,
                                                    const char* format_str,
                                                    int requested_channels);

// RateChangeWatcher

/**
 * @struct rate_change_watcher
 * @brief Opaque structure watching CoreAudio device sample rate changes.
 *
 * Watches kAudioDevicePropertyNominalSampleRate. Used by capture/playback
 * threads to detect rate changes and trigger engine rebuilds.
 */
typedef struct rate_change_watcher rate_change_watcher_t;

/**
 * @brief Create a rate change watcher.
 *
 * Installs a HAL listener on the device's nominal sample rate.
 *
 * @param device_id The HAL Device ID to watch.
 * @param expected_rate The rate the engine currently expects.
 * @return Pointer to the created watcher, or NULL on failure.
 */
rate_change_watcher_t* rate_change_watcher_create(AudioDeviceID device_id,
                                                  double expected_rate);

/**
 * @brief Check if a pending sample rate change has occurred.
 *
 * @param watcher Pointer to the rate change watcher.
 * @param out_rate Pointer to double to receive the new rate if changed.
 * @return true if a change occurred (and out_rate is set), false otherwise.
 */
bool rate_change_watcher_get_pending_change(rate_change_watcher_t* watcher,
                                            double* out_rate);

/**
 * @brief Dispose of the rate change watcher.
 *
 * Removes the HAL listener. Must be called before freeing.
 *
 * @param watcher Pointer to the rate change watcher.
 */
void rate_change_watcher_dispose(rate_change_watcher_t* watcher);

/**
 * @brief Destroy and free the rate change watcher.
 *
 * @param watcher Pointer to the rate change watcher to free.
 */
void rate_change_watcher_free(rate_change_watcher_t* watcher);

#endif  // ENABLE_COREAUDIO

#endif  // CLIB_BACKEND_CORE_AUDIO_DEVICE_H
