// Asynchronous windowed-sinc resampler.
//
// Same buffer layout, same `last_index` semantics, same `t_ratio` accumulation,
// same kernel decimation — output samples agree bit-for-bit (modulo the FMA-reduction
// order in the dot product, which is on the order of a few ULPs).
//
// Memory: every internal buffer is sized at init based on `chunkSize` and
// `maxRelativeRatio`. There is **no** dynamic allocation on the hot path.

#ifndef CLIB_RESAMPLER_ASYNC_SINC_RESAMPLER_H
#define CLIB_RESAMPLER_ASYNC_SINC_RESAMPLER_H

#include <stddef.h>
#include <stdbool.h>
#include "Audio/audio_chunk.h"
#include "resampler_error.h"
#include "sinc_window_function.h"
#include "sinc_dot_product.h"
#include "Config/resampler_config_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SINC_INTERPOLATION_LINEAR = 0,
    SINC_INTERPOLATION_QUADRATIC,
    SINC_INTERPOLATION_CUBIC
} sinc_interpolation_type_t;

typedef struct {
    size_t channels;
    size_t chunk_size;
    // Filter geometry.
    size_t sinc_len;
    size_t oversampling_factor;
    sinc_interpolation_type_t interpolation;
    // ramp toward the target ratio.
    double base_ratio;
    double resample_ratio;
    double target_ratio;
    double last_index; // tracking index
    // in the interpolator.
    double* sinc_table;
    // Per-channel input buffer. Layout:
    //   [0 .. 2*sincLen)            — history (last 2*sincLen samples of the
    //                                  previous chunk, or zeros initially)
    //   [2*sincLen .. 2*sincLen+chunkSize) — current chunk's data
    audio_buffers_t* input_buffer;
    // Pre-allocated scratch for per-frame `idx` values. Pre-computed once per
    // chunk so the per-channel loops can iterate without repeating the idx
    // accumulation.
    double* idx_scratch;
    double* frac_scratch;
    // Maximum output frames the resampler can ever produce in one call. The
    // caller uses this to size the output AudioChunk once at startup.
    size_t max_output_frames;
} async_sinc_resampler_t;

async_sinc_resampler_t* async_sinc_resampler_create(size_t channels, size_t input_rate, size_t output_rate, size_t sinc_len, size_t oversampling_factor, sinc_interpolation_type_t interpolation, window_function_t window, double f_cutoff, bool has_f_cutoff, size_t chunk_size, double max_relative_ratio);
async_sinc_resampler_t* async_sinc_resampler_create_from_profile(size_t channels, size_t input_rate, size_t output_rate, resampler_profile_t profile, size_t chunk_size, double max_relative_ratio);
void async_sinc_resampler_free(async_sinc_resampler_t* resampler);

resampler_error_t async_sinc_resampler_process(async_sinc_resampler_t* resampler, const audio_chunk_t* input, audio_chunk_t* output);
void async_sinc_resampler_set_relative_ratio(async_sinc_resampler_t* resampler, double multiplier);
double async_sinc_resampler_get_ratio(const async_sinc_resampler_t* resampler);
size_t async_sinc_resampler_get_max_output_frames(const async_sinc_resampler_t* resampler);
size_t async_sinc_resampler_get_chunk_size(const async_sinc_resampler_t* resampler);
size_t async_sinc_resampler_get_channels(const async_sinc_resampler_t* resampler);

#ifdef __cplusplus
}
#endif

#endif // CLIB_RESAMPLER_ASYNC_SINC_RESAMPLER_H
