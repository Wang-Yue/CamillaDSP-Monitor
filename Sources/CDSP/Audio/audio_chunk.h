// Non-interleaved float buffers, one vector per channel.

#ifndef CLIB_AUDIO_AUDIO_CHUNK_H
#define CLIB_AUDIO_AUDIO_CHUNK_H

#include "audio_buffers.h"

/// A chunk of non-interleaved audio data flowing through the pipeline.
///
/// Storage is heap-backed (`audio_buffers_t`) so per-channel mutable pointers
/// stay stable across struct copies and the audio thread can mutate samples
/// without going through copy-on-write uniqueness checks. Two `audio_chunk_t`
/// values that share an `audio_buffers_t` see the same samples — this is a
/// deliberate trade against value semantics, made to
/// remove allocations on the hot path.
typedef struct audio_chunk audio_chunk_t;
typedef struct round_robin_chunk_pool round_robin_chunk_pool_t;

/// Create a new silent audio chunk with freshly allocated storage.
audio_chunk_t* audio_chunk_create(size_t frames, size_t channels);
/// Create an audio chunk that adopts the given `audio_buffers_t`. Zero-copy.
audio_chunk_t* audio_chunk_from_buffers(audio_buffers_t* buffers,
                                        size_t valid_frames);
void audio_chunk_free(audio_chunk_t* chunk);

/// Per-channel sample capacity.
size_t audio_chunk_get_frames(const audio_chunk_t* chunk);

/// Number of channels.
size_t audio_chunk_get_channels(const audio_chunk_t* chunk);

/// Direct mutable per-channel pointer. The pointer is stable for the
/// lifetime of the underlying `audio_buffers_t` and aliases across struct
/// copies — no CoW.
mutable_waveform_t audio_chunk_get_channel(const audio_chunk_t* chunk, size_t ch);

/// Number of valid frames (may be < `frames` at end-of-stream).
size_t audio_chunk_get_valid_frames(const audio_chunk_t* chunk);
void audio_chunk_set_valid_frames(audio_chunk_t* chunk, size_t valid_frames);

/// Get the underlying audio buffers.
audio_buffers_t* audio_chunk_get_buffers(audio_chunk_t* chunk);

round_robin_chunk_pool_t* round_robin_chunk_pool_create(size_t capacity,
                                                        size_t frames,
                                                        size_t channels);
/// Retrieves the next available unique chunk buffer from the pool.
audio_chunk_t* round_robin_chunk_pool_next(round_robin_chunk_pool_t* pool);
void round_robin_chunk_pool_free(round_robin_chunk_pool_t* pool);

#endif  // CLIB_AUDIO_AUDIO_CHUNK_H
