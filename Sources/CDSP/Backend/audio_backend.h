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

typedef struct capture_backend capture_backend_t;
typedef struct playback_backend playback_backend_t;

/// Protocol for audio capture backends
typedef struct {
  /// Open the capture device
  bool (*open)(void* ctx, backend_error_t* err);
  /// Read a chunk of audio into the provided buffer. Returns false on
  /// end-of-stream or no data.
  bool (*read)(void* ctx, size_t frames, audio_chunk_t* chunk,
               backend_error_t* err);
  /// Close the capture device
  void (*close)(void* ctx);
  /// New nominal sample rate detected on the device since `open()`,
  /// or `nil` if the rate is still the one we asked for. Polled by
  /// the engine each chunk to surface
  /// `ProcessingStopReason.captureFormatChange` when a user (or
  /// another app) flips the device rate at runtime.
  bool (*get_pending_rate_change)(void* ctx, double* out_rate);
  /// `true` when the capture device exposes a tunable clock — at
  /// the moment that's BlackHole 0.5.0+ on macOS, which advertises
  /// an "Internal Adjustable" clock source. When `true`, the
  /// rate-adjust loop sends corrections through `setPitch(_:)`
  /// instead of nudging the resampler ratio (bit-perfect path).
  bool (*is_pitch_control_supported)(void* ctx);
  /// Apply a clock-pitch correction to the capture device.
  /// `multiplier` is close to `1.0` (typically `1.0 ± 0.001`).
  /// No-op for backends without tunable clocks.
  void (*set_pitch)(void* ctx, double multiplier);
  /// Wait for new samples to become available, up to the given timeout.
  bool (*wait_for_data)(void* ctx, uint32_t timeout_ms);
  void (*destroy)(void* ctx);
} capture_backend_vtable_t;

/// Protocol for audio capture backends
struct capture_backend {
  void* ctx;
  const capture_backend_vtable_t* vtable;
};

/// Protocol for audio playback backends
typedef struct {
  /// Open the playback device
  bool (*open)(void* ctx, backend_error_t* err);
  /// Write a chunk of audio
  bool (*write)(void* ctx, const audio_chunk_t* chunk, backend_error_t* err);
  /// Close the playback device
  void (*close)(void* ctx);
  /// Get the current playback buffer level in samples
  size_t (*get_buffer_level)(void* ctx);
  /// See `CaptureBackend.pendingRateChange`. Used to surface
  /// `ProcessingStopReason.playbackFormatChange`.
  bool (*get_pending_rate_change)(void* ctx, double* out_rate);
  /// Push `frames` zero samples per channel into the output ring
  /// before the engine's first real chunk arrives. Used at startup
  /// so the rate-adjust controller sees a buffer level near
  /// `target_level` from its first measurement, instead of having
  /// to ramp up from empty.
  bool (*prefill_silence)(void* ctx, size_t frames, backend_error_t* err);
  /// Flag to indicate if the playback is currently paused, to suppress underrun
  /// warnings.
  bool (*get_is_paused)(void* ctx);
  /// Flag to indicate if the playback is currently paused, to suppress underrun
  /// warnings.
  void (*set_is_paused)(void* ctx, bool paused);
  void (*destroy)(void* ctx);
} playback_backend_vtable_t;

/// Protocol for audio playback backends
struct playback_backend {
  void* ctx;
  const playback_backend_vtable_t* vtable;
};

typedef struct processing_parameters processing_parameters_t;

// Factory functions
/// Create a capture backend instance based on the configuration.
capture_backend_t* create_capture_backend(const capture_device_config_t* config,
                                          int sample_rate, int chunk_size,
                                          processing_parameters_t* params,
                                          backend_error_t* err);
/// Create a playback backend instance based on the configuration.
playback_backend_t* create_playback_backend(
    const playback_device_config_t* config, int sample_rate, int chunk_size,
    processing_parameters_t* params, backend_error_t* err);

// CaptureBackend wrapper methods
/// Open the capture device
bool capture_backend_open(capture_backend_t* backend, backend_error_t* err);
/// Read a chunk of audio into the provided buffer. Returns false on
/// end-of-stream or no data.
bool capture_backend_read(capture_backend_t* backend, size_t frames,
                          audio_chunk_t* chunk, backend_error_t* err);
/// Close the capture device
void capture_backend_close(capture_backend_t* backend);
/// Get pending sample rate change detected on the capture device.
bool capture_backend_get_pending_rate_change(capture_backend_t* backend,
                                             double* out_rate);
/// Check if the capture device supports clock-pitch correction.
bool capture_backend_pitch_control_supported(capture_backend_t* backend);
/// Apply a clock-pitch correction to the capture device.
void capture_backend_set_pitch(capture_backend_t* backend, double multiplier);
/// Wait for new samples to become available, up to the given timeout.
bool capture_backend_wait(capture_backend_t* backend, uint32_t timeout_ms);
/// Destroy and free the capture backend.
void capture_backend_free(capture_backend_t* backend);

// PlaybackBackend wrapper methods
/// Open the playback device
bool playback_backend_open(playback_backend_t* backend, backend_error_t* err);
/// Write a chunk of audio
bool playback_backend_write(playback_backend_t* backend,
                            const audio_chunk_t* chunk, backend_error_t* err);
/// Close the playback device
void playback_backend_close(playback_backend_t* backend);
/// Get the current playback buffer level in samples
size_t playback_backend_get_buffer_level(playback_backend_t* backend);
/// Get pending sample rate change detected on the playback device.
bool playback_backend_get_pending_rate_change(playback_backend_t* backend,
                                              double* out_rate);
/// Push zero samples per channel into the output ring before first real chunk
/// arrives.
bool playback_backend_prefill_silence(playback_backend_t* backend,
                                      size_t frames, backend_error_t* err);
/// Get paused flag status.
bool playback_backend_get_is_paused(playback_backend_t* backend);
/// Set paused flag status.
void playback_backend_set_is_paused(playback_backend_t* backend, bool paused);
/// Destroy and free the playback backend.
void playback_backend_free(playback_backend_t* backend);

#endif  // CLIB_BACKEND_AUDIO_BACKEND_H
