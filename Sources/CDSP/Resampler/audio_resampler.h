// Resampler protocol + shared types.
// Four resampler implementations conform to AudioResampler:
//   * SynchronousResampler — FFT-based fixed-ratio.
//   * AsyncSincResampler   — Asynchronous windowed-sinc resampler.
//   * AsyncPolyResampler   — Asynchronous polynomial resampler.
//   * AppleResampler       — Core Audio AudioConverter wrapper.

#ifndef CLIB_RESAMPLER_AUDIO_RESAMPLER_H
#define CLIB_RESAMPLER_AUDIO_RESAMPLER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Config/resampler_config_types.h"
#include "async_poly_resampler.h"
#include "async_sinc_resampler.h"
#include "resampler_error.h"
#include "synchronous_resampler.h"
#if defined(__APPLE__)
#include "apple_resampler.h"
#endif

typedef enum {
  RESAMPLER_IMPL_SYNCHRONOUS = 0,
  RESAMPLER_IMPL_ASYNC_SINC,
  RESAMPLER_IMPL_ASYNC_POLY,
#if defined(__APPLE__)
  RESAMPLER_IMPL_APPLE
#endif
} resampler_impl_type_t;

/// Resampler protocol.
///
/// Each resampler is initialised with a *base* ratio of `outputRate /
/// inputRate`, a *fixed* `chunkSize` (the number of input frames every
/// `process` call must receive), and a *relative* multiplier (`1.0` by default)
/// that the rate-adjust loop nudges to track clock drift. The effective ratio
/// per chunk is `base * relative`.
///
/// Because `chunkSize` is fixed at construction, the implementations
/// pre-allocate every internal buffer at init and never allocate on the hot
/// path. The caller must supply pre-allocated output buffers via
/// `process(input:into:)`.
typedef struct audio_resampler {
  resampler_impl_type_t type;
  void* impl;
  resampler_error_t (*process)(struct audio_resampler* self,
                               const audio_chunk_t* input,
                               audio_chunk_t* output);
  void (*set_relative_ratio)(struct audio_resampler* self, double multiplier);
  double (*get_ratio)(const struct audio_resampler* self);
  size_t (*get_max_output_frames)(const struct audio_resampler* self);
  size_t (*get_chunk_size)(const struct audio_resampler* self);
  size_t (*get_channels)(const struct audio_resampler* self);
  void (*free)(struct audio_resampler* self);
} audio_resampler_t;

audio_resampler_t* audio_resampler_create_from_config(
    const resampler_config_t* config, size_t input_rate, size_t output_rate,
    size_t channels, size_t chunk_size);

audio_resampler_t* audio_resampler_wrap_synchronous(
    synchronous_resampler_t* res);
audio_resampler_t* audio_resampler_wrap_async_sinc(async_sinc_resampler_t* res);
audio_resampler_t* audio_resampler_wrap_async_poly(async_poly_resampler_t* res);
#if defined(__APPLE__)
audio_resampler_t* audio_resampler_wrap_apple(apple_resampler_t* res);
#endif

/// Zero-allocation API. The caller pre-allocates `output` with
/// `output.channels == channels` and `output.frames >= maxOutputFrames`.
/// The resampler writes the produced samples in place and updates
/// `output.validFrames`.
///
/// Throws `ResamplerError.inputSizeMismatch` if `input.validFrames` does
/// not equal `chunkSize`, `outputBufferTooSmall` if the per-channel buffers
/// can't fit the output, or `channelCountMismatch` if the channel layout
/// doesn't match.
static inline resampler_error_t audio_resampler_process(
    audio_resampler_t* resampler, const audio_chunk_t* input,
    audio_chunk_t* output) {
  if (!resampler || !resampler->process) return RESAMPLER_ERR_INVALID_PARAMETER;
  return resampler->process(resampler, input, output);
}

/// Apply a multiplicative correction on top of the base ratio.
/// `SynchronousResampler` ignores this (its ratio is fixed by
/// construction).
static inline void audio_resampler_set_relative_ratio(
    audio_resampler_t* resampler, double multiplier) {
  if (resampler && resampler->set_relative_ratio) {
    resampler->set_relative_ratio(resampler, multiplier);
  }
}

/// Current effective ratio (`base * relative`).
static inline double audio_resampler_get_ratio(
    const audio_resampler_t* resampler) {
  return (resampler && resampler->get_ratio) ? resampler->get_ratio(resampler)
                                             : 1.0;
}

/// Worst-case output frames across the resampler's allowed ratio range —
/// use this to size the output `AudioChunk` once at startup.
static inline size_t audio_resampler_get_max_output_frames(
    const audio_resampler_t* resampler) {
  return (resampler && resampler->get_max_output_frames)
             ? resampler->get_max_output_frames(resampler)
             : 0;
}

/// Input frames the resampler expects on every `process` call.
static inline size_t audio_resampler_get_chunk_size(
    const audio_resampler_t* resampler) {
  return (resampler && resampler->get_chunk_size)
             ? resampler->get_chunk_size(resampler)
             : 0;
}

/// Number of channels processed per call.
static inline size_t audio_resampler_get_channels(
    const audio_resampler_t* resampler) {
  return (resampler && resampler->get_channels)
             ? resampler->get_channels(resampler)
             : 0;
}

static inline void audio_resampler_free(audio_resampler_t* resampler) {
  if (resampler && resampler->free) {
    resampler->free(resampler);
  }
}

sinc_interpolation_type_t sinc_interpolation_type_from_string(const char* str);
/// Polynomial degree exposed by `AsyncPolyResampler`.
poly_interpolation_t poly_interpolation_from_string(const char* str);

#endif  // CLIB_RESAMPLER_AUDIO_RESAMPLER_H
