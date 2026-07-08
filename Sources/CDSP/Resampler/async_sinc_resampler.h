// Asynchronous windowed-sinc resampler.
//
// Same buffer layout, same `last_index` semantics, same `t_ratio` accumulation,
// same kernel decimation — output samples agree bit-for-bit (modulo the
// FMA-reduction order in the dot product, which is on the order of a few ULPs).
//
// Memory: every internal buffer is sized at init based on `chunkSize` and
// `maxRelativeRatio`. There is **no** dynamic allocation on the hot path.

#ifndef CLIB_RESAMPLER_ASYNC_SINC_RESAMPLER_H
#define CLIB_RESAMPLER_ASYNC_SINC_RESAMPLER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Config/resampler_config_types.h"
#include "resampler_error.h"
#include "sinc_dot_product.h"
#include "sinc_window_function.h"

typedef enum {
  SINC_INTERPOLATION_NEAREST = 0,
  SINC_INTERPOLATION_LINEAR,
  SINC_INTERPOLATION_QUADRATIC,
  SINC_INTERPOLATION_CUBIC
} sinc_interpolation_type_t;

struct async_sinc_resampler;
typedef struct async_sinc_resampler async_sinc_resampler_t;

async_sinc_resampler_t* async_sinc_resampler_create(
    size_t channels, size_t input_rate, size_t output_rate, size_t sinc_len,
    size_t oversampling_factor, sinc_interpolation_type_t interpolation,
    window_function_t window, double f_cutoff, bool has_f_cutoff,
    size_t chunk_size, double max_relative_ratio);
async_sinc_resampler_t* async_sinc_resampler_create_from_profile(
    size_t channels, size_t input_rate, size_t output_rate,
    resampler_profile_t profile, size_t chunk_size, double max_relative_ratio);
void async_sinc_resampler_free(async_sinc_resampler_t* resampler);

resampler_error_t async_sinc_resampler_process(
    async_sinc_resampler_t* resampler, const audio_chunk_t* input,
    audio_chunk_t* output);
void async_sinc_resampler_set_relative_ratio(async_sinc_resampler_t* resampler,
                                             double multiplier);
double async_sinc_resampler_get_ratio(const async_sinc_resampler_t* resampler);
size_t async_sinc_resampler_get_max_output_frames(
    const async_sinc_resampler_t* resampler);
size_t async_sinc_resampler_get_chunk_size(
    const async_sinc_resampler_t* resampler);
size_t async_sinc_resampler_get_channels(
    const async_sinc_resampler_t* resampler);

#endif  // CLIB_RESAMPLER_ASYNC_SINC_RESAMPLER_H
