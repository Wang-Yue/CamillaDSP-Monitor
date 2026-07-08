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

/// Direction marker for HAL device queries. The
/// `kAudioDevicePropertyScopeInput` and `kAudioDevicePropertyScopeOutput`
/// constants are aliases of `kAudioObjectPropertyScopeInput`/`Output`, so the
/// same value works for stream-config queries and stream-list queries.
typedef enum {
  CORE_AUDIO_SCOPE_INPUT = 0,
  CORE_AUDIO_SCOPE_OUTPUT = 1
} core_audio_scope_t;

/// Pure-helper namespace for enumerating and identifying CoreAudio HAL
/// devices. None of the methods here mutate state; they're safe to call
/// concurrently.
typedef struct {
  AudioDeviceID id;
  char name[256];
} core_audio_device_info_t;

// MARK: - Enumeration

/// Every HAL device on the system, regardless of stream direction.
int core_audio_device_all_ids(AudioDeviceID* out_ids, int max_ids);
/// User-facing name of a device, or `nil` if the lookup fails.
bool core_audio_device_name(AudioDeviceID device_id, char* out_name,
                            size_t max_len);
/// True if the device exposes any streams in the given direction.
bool core_audio_device_has_stream(AudioDeviceID device_id,
                                  core_audio_scope_t scope);
/// HAL stream IDs for the given device + direction.
int core_audio_device_streams(AudioDeviceID device_id, core_audio_scope_t scope,
                              AudioStreamID* out_streams, int max_streams);
/// Devices that have at least one stream in the requested direction,
/// each paired with its user-facing name. Devices that fail the
/// stream-config check (e.g. an output-only device queried in
/// `.input` scope) are filtered out.
int core_audio_device_list_devices(core_audio_scope_t scope,
                                   core_audio_device_info_t* out_devices,
                                   int max_devices);

// MARK: - Lookup

/// HAL ID of the system-default device for the given direction.
AudioDeviceID core_audio_device_default_id(core_audio_scope_t scope);
/// HAL ID of a named device, or the system default when `name` is
/// `nil`. Returns `nil` if the named device can't be found.
AudioDeviceID core_audio_device_id_for_name(const char* name,
                                            core_audio_scope_t scope);

// MARK: - Sample-rate control

/// Push the device's nominal sample rate, then poll until the
/// change has been committed. CoreAudio applies the change
/// asynchronously on a HAL thread; if we proceed straight to
/// `AudioUnitInitialize` the AudioUnit can latch the *old* rate
/// and silently sample-rate-convert from then on. Returns `true`
/// only when both the set call succeeded *and* the device's
/// reported rate matches `rate` within `~0.5 Hz` after the poll.
///
/// Devices that don't support the requested rate return a
/// non-zero status from the set call; we surface that as `false`
/// without polling.
bool core_audio_device_set_nominal_sample_rate(AudioDeviceID device_id,
                                               double rate);
/// Read the device's current nominal sample rate. Used to verify
/// that `setNominalSampleRate` actually took effect — CoreAudio
/// applies the change asynchronously, so callers should poll this
/// for a short window before falling back.
bool core_audio_device_get_nominal_sample_rate(AudioDeviceID device_id,
                                               double* out_rate);

// MARK: - Buffer frame size control

/// Set the device's buffer frame size for a given scope. Returns `true` on
/// success.
bool core_audio_device_set_buffer_frame_size(AudioDeviceID device_id,
                                             uint32_t frames,
                                             core_audio_scope_t scope);
/// Read the device's current buffer frame size for a given scope.
bool core_audio_device_get_buffer_frame_size(AudioDeviceID device_id,
                                             core_audio_scope_t scope,
                                             uint32_t* out_frames);

// MARK: - Clock-source / pitch control (BlackHole 0.5.0+)

/// Set the device's active clock source by ID. Returns `true` on success.
bool core_audio_device_set_clock_source_id(AudioDeviceID device_id,
                                           uint32_t source_id);
/// If `deviceID` advertises an "Internal Adjustable" clock source
/// (BlackHole 0.5.0+), select it as the active source and return
/// `true`. Returns `false` for devices that don't support pitch
/// tuning.
bool core_audio_device_select_adjustable_clock_source(AudioDeviceID device_id);
/// Apply a clock-pitch correction to the capture device by
/// writing `kAudioDevicePropertyStereoPan`. Upstream maps
/// `pitch ∈ [0.99, 1.01]` to `pan ∈ [0, 1]` with the formula
/// `pan = (pitch - 1.0) * 50.0 + 0.5`, clamped to the valid
/// range.
void core_audio_device_set_pitch(AudioDeviceID device_id, double pitch);
/// Returns true if the device exposes the nominal-sample-rate
/// property — needed before installing a `RateChangeWatcher` so we
/// don't churn HAL listener registrations on devices that can't
/// publish rate changes anyway.
bool core_audio_device_has_nominal_sample_rate_property(
    AudioDeviceID device_id);

// MARK: - Stream-format builder

/// Standard 32-bit linear-PCM ASBD used by both backends. Pass
/// `interleaved: false` for the non-interleaved layout the engine
/// prefers (one HAL buffer per channel); `true` for the classic
/// interleaved fallback (one buffer with all channels packed).
AudioStreamBasicDescription core_audio_device_float32_stream_format(
    double sample_rate, int channels, bool interleaved);

bool core_audio_device_set_matching_physical_format(AudioDeviceID device_id,
                                                    core_audio_scope_t scope,
                                                    double sample_rate,
                                                    const char* format_str,
                                                    int requested_channels);

// RateChangeWatcher
/// Watches a CoreAudio device's `kAudioDevicePropertyNominalSampleRate`
/// and reports any change away from the rate the engine asked for.
///
/// The capture thread polls
/// `pendingRateChange` once per chunk; on a real change it stops the
/// engine with `.captureFormatChange(rate)` / `.playbackFormatChange(rate)`
/// so the host can rebuild at the new rate.
///
/// Lifetime: created by `CoreAudioCapture.open()` /
/// `CoreAudioPlayback.open()` *after* `setNominalSampleRate` has been
/// applied (so the watcher's expected rate is the one we just pushed).
/// `dispose()` removes the HAL listener and must run before the owner
/// is deallocated — `deinit` calls it as a backstop.
typedef struct rate_change_watcher rate_change_watcher_t;

/// Create a rate change watcher for the specified device and expected rate.
rate_change_watcher_t* rate_change_watcher_create(AudioDeviceID device_id,
                                                  double expected_rate);
/// Check if a pending sample rate change has occurred.
bool rate_change_watcher_get_pending_change(rate_change_watcher_t* watcher,
                                            double* out_rate);
/// Dispose of the rate change watcher by removing HAL listeners.
void rate_change_watcher_dispose(rate_change_watcher_t* watcher);
/// Destroy and free the rate change watcher.
void rate_change_watcher_free(rate_change_watcher_t* watcher);

#endif  // ENABLE_COREAUDIO

#endif  // CLIB_BACKEND_CORE_AUDIO_DEVICE_H
