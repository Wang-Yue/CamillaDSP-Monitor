// Heap-allocated, contiguous per-channel audio storage.
//
// Replaces nested array/pointer-to-pointer ("array of arrays") chunk storage.
// The 2-D nested layout had two costs the audio thread couldn't afford:
//
//   1. Uniqueness/copy-on-write checks or fragmented heap access on inner
//   buffers;
//      whenever any external reference kept an inner buffer shared (closures,
//      queues, captures), mutable access risked malloc'ing a fresh copy or
//      causing cache misses.
//   2. Element copies bumped per-channel buffer refcounts or overhead
//      on every value-copy of an audio chunk.
//
// `audio_buffers_t` allocates one contiguous block of `channels * capacity`
// `double` values up front and exposes per-channel `mutable_waveform_t`
// (pointer) views that are stable for the buffer's lifetime. The hot path uses
// the pointers directly — no pointer round trips, no ownership/uniqueness
// checks.
//
// Thread-safety: `audio_buffers_t` itself does no synchronisation. The pipeline
// already enforces single-writer discipline (the audio thread owns each
// buffer while it processes a chunk), so lock-free single-owner usage is honest
// here — the type is safe under pipeline discipline without locking.

#ifndef CLIB_AUDIO_AUDIO_BUFFERS_H
#define CLIB_AUDIO_AUDIO_BUFFERS_H

#include <stddef.h>

#include "double_helpers.h"

/// Contiguous, per-channel audio storage backed by a single heap allocation.
typedef struct audio_buffers audio_buffers_t;

/// Allocate a fresh buffer pool, zero-initialised.
audio_buffers_t* audio_buffers_create(size_t channels, size_t capacity);
/// Allocate a fresh buffer pool and copy existing waveform data into it.
audio_buffers_t* audio_buffers_copy_from(const double* const* waveforms,
                                         const size_t* channel_lengths,
                                         size_t channels);
void audio_buffers_free(audio_buffers_t* buffers);

/// Number of channels in the buffer.
size_t audio_buffers_get_channels(const audio_buffers_t* buffers);
/// Capacity in samples per channel.
size_t audio_buffers_get_capacity(const audio_buffers_t* buffers);

/// Mutable per-channel pointer. The pointer is stable for the lifetime of the
/// `audio_buffers_t`; callers may cache it.
mutable_waveform_t audio_buffers_get_channel(const audio_buffers_t* buffers,
                                             size_t ch);

#endif  // CLIB_AUDIO_AUDIO_BUFFERS_H
