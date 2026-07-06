// Heap-allocated, contiguous per-channel audio storage.
//
// Replaces nested array/pointer-to-pointer ("array of arrays") chunk storage. The 2-D
// nested layout had two costs the audio thread couldn't afford:
//
//   1. Uniqueness/copy-on-write checks or fragmented heap access on inner buffers;
//      whenever any external reference kept an inner buffer shared (closures, queues, captures),
//      mutable access risked malloc'ing a fresh copy or causing cache misses.
//   2. Element copies bumped per-channel buffer refcounts or overhead
//      on every value-copy of an audio chunk.
//
// `audio_buffers_t` allocates one contiguous block of `channels * capacity`
// `prc_fmt_t` values up front and exposes per-channel `mutable_waveform_t` (pointer)
// views that are stable for the buffer's lifetime. The hot path uses the
// pointers directly — no pointer round trips, no ownership/uniqueness checks.
//
// Thread-safety: `audio_buffers_t` itself does no synchronisation. The pipeline
// already enforces single-writer discipline (the audio thread owns each
// buffer while it processes a chunk), so lock-free single-owner usage is honest
// here — the type is safe under pipeline discipline without locking.

#ifndef CLIB_AUDIO_AUDIO_BUFFERS_H
#define CLIB_AUDIO_AUDIO_BUFFERS_H

#include "prc_fmt.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Contiguous, per-channel audio storage backed by a single heap allocation.
typedef struct {
    /// Number of channels.
    size_t channels;
    /// Per-channel capacity in `prc_fmt_t` samples.
    size_t capacity;
    /// One contiguous `channels * capacity` block. Channel `ch` lives at `[ch * capacity ..< (ch + 1) * capacity]`.
    prc_fmt_t* storage;
    /// Pre-built per-channel views — sized to `capacity`, pointing into `storage`. Built once at init and never resized; the pointers stay valid for the entire lifetime of this `audio_buffers_t`.
    mutable_waveform_t* channel_buffers;
} audio_buffers_t;

/// Allocate a fresh buffer pool, zero-initialised.
audio_buffers_t* audio_buffers_create(size_t channels, size_t capacity);
/// Allocate a fresh buffer pool and copy existing waveform data into it.
audio_buffers_t* audio_buffers_copy_from(const prc_fmt_t* const* waveforms, const size_t* channel_lengths, size_t channels);
void audio_buffers_free(audio_buffers_t* buffers);

/// Mutable per-channel pointer. The pointer is stable for the lifetime of the `audio_buffers_t`; callers may cache it.
static inline mutable_waveform_t audio_buffers_get_channel(const audio_buffers_t* buffers, size_t ch) {
    return buffers->channel_buffers[ch];
}

#ifdef __cplusplus
}
#endif

#endif // CLIB_AUDIO_AUDIO_BUFFERS_H
