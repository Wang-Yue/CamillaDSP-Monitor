// AudioHistoryBuffer — stores recent audio samples for spectrum analysis and vector scope.

#ifndef CLIB_AUDIO_AUDIO_HISTORY_BUFFER_H
#define CLIB_AUDIO_AUDIO_HISTORY_BUFFER_H

#include "Audio/lock_free_ring_buffer.h"
#include "Audio/audio_chunk.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Maximum number of frames retained per channel. At 48 kHz that's roughly 5.5 s of audio
/// — enough headroom for an FFT down to ~5 Hz.
#define AUDIO_HISTORY_BUFFER_CAPACITY 262144

/// Status codes returned by `audio_history_buffer_read_latest`. The buffer is
/// general-purpose (used by spectrum analysis *and* level/sample queries), so
/// it owns its own error/status type rather than borrowing from the spectrum
/// analyzer.
typedef enum {
    AUDIO_HISTORY_BUFFER_OK = 0,
    /// `reset(channels)` has not been called, or was called with `0`.
    AUDIO_HISTORY_BUFFER_ERROR_EMPTY = -1,
    /// Caller asked for a channel index outside `0..<channels`.
    AUDIO_HISTORY_BUFFER_ERROR_OUT_OF_RANGE = -2
} audio_history_buffer_status_t;

/// Owns one `spsc_audio_ring_buffer_t` per channel for one side (capture or
/// playback) of the engine. Resized only between engine starts, when no
/// audio thread is running. Read by consumers via `read_latest(...)`
/// (snapshot semantics — same window can be re-read for FFTs at
/// different lengths), optionally averaging across channels.
typedef struct {
    size_t channels;
    spsc_audio_ring_buffer_t** buffers;
    /// Preallocated scratch used by the consumer to average channels
    /// without per-call heap traffic. Sized to the ring's capacity.
    float* averaging_scratch;
} audio_history_buffer_t;

audio_history_buffer_t* audio_history_buffer_create(void);
/// Re-allocate buffers for a new channel layout. Must only be called
/// while the engine is stopped (no producer touching the ring).
void audio_history_buffer_reset(audio_history_buffer_t* history, size_t channels);
void audio_history_buffer_free(audio_history_buffer_t* history);

/// Whether any sample has been written on this side yet.
bool audio_history_buffer_has_data(const audio_history_buffer_t* history);
/// **Producer-only.** Forward each channel's waveform into the matching
/// lock-free ring.
void audio_history_buffer_append(audio_history_buffer_t* history, const audio_chunk_t* chunk);

/// **Consumer.** Copy the most recent `count` samples for the given
/// channel into `dest`. When `channel` is negative (`-1`), all channels are
/// averaged into `dest`. Returns status code and sets `*enough_data` to `false` if there isn't enough data
/// yet.
///
/// `dest` must have capacity for at least `count` floats.
audio_history_buffer_status_t audio_history_buffer_read_latest(
    const audio_history_buffer_t* history,
    float* dest,
    size_t count,
    int channel,
    bool* enough_data
);

#ifdef __cplusplus
}
#endif

#endif // CLIB_AUDIO_AUDIO_HISTORY_BUFFER_H
