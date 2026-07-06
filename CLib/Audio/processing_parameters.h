// Concurrency model
// -----------------
// Every field is backed by lock-free atomics (`atomic_double_t` or `_Atomic bool`)
// — no mutexes or locks.
// Target volume, current volume, and mute states are kept for 5 faders (Main, Aux 1-4)
// as separate inline atomic variables to avoid heap allocation and conform to real-time requirements.

#ifndef CLIB_AUDIO_PROCESSING_PARAMETERS_H
#define CLIB_AUDIO_PROCESSING_PARAMETERS_H

#include "Audio/prc_fmt.h"
#include "Audio/lock_free_ring_buffer.h"
#include "Audio/audio_chunk.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FADER_T_DEFINED
#define FADER_T_DEFINED
typedef enum {
    FADER_MAIN = 0,
    FADER_AUX1 = 1,
    FADER_AUX2 = 2,
    FADER_AUX3 = 3,
    FADER_AUX4 = 4
} fader_t;
#endif
#define FADER_COUNT 5

/// Default volume (dB) when an engine starts.
#define PROCESSING_PARAMETERS_DEFAULT_VOLUME 0.0
/// Default mute state.
#define PROCESSING_PARAMETERS_DEFAULT_MUTE false

// MARK: - Storage

typedef struct {
    /// Target volume (dB) for fader 0-4 — what the user has asked for. UI thread writes; VolumeFilter reads on every chunk.
    atomic_double_t target_volumes[FADER_COUNT];
    /// Current volume (dB) for fader 0-4 — tracking ramp progress.
    atomic_double_t current_volumes[FADER_COUNT];
    /// Mute state for fader 0-4. UI writes; VolumeFilter reads each chunk.
    _Atomic bool muted[FADER_COUNT];

    size_t capture_channels;
    size_t playback_channels;
    /// Per-channel signal levels (dB).
    atomic_double_t* capture_signal_peak;
    atomic_double_t* capture_signal_rms;
    atomic_double_t* playback_signal_peak;
    atomic_double_t* playback_signal_rms;
} processing_parameters_t;

processing_parameters_t* processing_parameters_create(size_t capture_channels, size_t playback_channels);
void processing_parameters_free(processing_parameters_t* params);

prc_fmt_t processing_parameters_get_target_volume_for_fader(const processing_parameters_t* params, fader_t fader);
void processing_parameters_set_target_volume_for_fader(processing_parameters_t* params, prc_fmt_t value, fader_t fader);
prc_fmt_t processing_parameters_get_current_volume_for_fader(const processing_parameters_t* params, fader_t fader);
void processing_parameters_set_current_volume_for_fader(processing_parameters_t* params, prc_fmt_t value, fader_t fader);
bool processing_parameters_is_muted_for_fader(const processing_parameters_t* params, fader_t fader);
void processing_parameters_set_muted_for_fader(processing_parameters_t* params, bool value, fader_t fader);

static inline prc_fmt_t processing_parameters_get_target_volume(const processing_parameters_t* params) {
    return processing_parameters_get_target_volume_for_fader(params, FADER_MAIN);
}
static inline void processing_parameters_set_target_volume(processing_parameters_t* params, prc_fmt_t value) {
    processing_parameters_set_target_volume_for_fader(params, value, FADER_MAIN);
}
static inline prc_fmt_t processing_parameters_get_current_volume(const processing_parameters_t* params) {
    return processing_parameters_get_current_volume_for_fader(params, FADER_MAIN);
}
static inline void processing_parameters_set_current_volume(processing_parameters_t* params, prc_fmt_t value) {
    processing_parameters_set_current_volume_for_fader(params, value, FADER_MAIN);
}
static inline bool processing_parameters_is_muted(const processing_parameters_t* params) {
    return processing_parameters_is_muted_for_fader(params, FADER_MAIN);
}
static inline void processing_parameters_set_muted(processing_parameters_t* params, bool value) {
    processing_parameters_set_muted_for_fader(params, value, FADER_MAIN);
}

void processing_parameters_get_capture_signal_peak(const processing_parameters_t* params, prc_fmt_t* out_levels, size_t count);
void processing_parameters_set_capture_signal_peak(processing_parameters_t* params, const prc_fmt_t* levels, size_t count);
void processing_parameters_get_capture_signal_rms(const processing_parameters_t* params, prc_fmt_t* out_levels, size_t count);
void processing_parameters_set_capture_signal_rms(processing_parameters_t* params, const prc_fmt_t* levels, size_t count);

void processing_parameters_get_playback_signal_peak(const processing_parameters_t* params, prc_fmt_t* out_levels, size_t count);
void processing_parameters_set_playback_signal_peak(processing_parameters_t* params, const prc_fmt_t* levels, size_t count);
void processing_parameters_get_playback_signal_rms(const processing_parameters_t* params, prc_fmt_t* out_levels, size_t count);
void processing_parameters_set_playback_signal_rms(processing_parameters_t* params, const prc_fmt_t* levels, size_t count);

// MARK: - Chunk-based updates (no-allocation, audio-thread safe)

/// Asynchronously update the capture-side peak and RMS levels on the audio thread.
/// Does not allocate.
prc_fmt_t processing_parameters_update_capture_levels(processing_parameters_t* params, const audio_chunk_t* chunk);
/// Asynchronously update the playback-side peak and RMS levels on the audio thread.
/// Does not allocate.
prc_fmt_t processing_parameters_update_playback_levels(processing_parameters_t* params, const audio_chunk_t* chunk);

#ifdef __cplusplus
}
#endif

#endif // CLIB_AUDIO_PROCESSING_PARAMETERS_H
