// Asynchronous polynomial resampler.
//
// Same buffer layout, same `last_index`
// semantics, same `t_ratio` accumulation, same Newton-form polynomial
// formulas — output samples agree bit-for-bit.
//
// No anti-aliasing; for
// quality use `AsyncSincResampler`.
//
// Memory: every internal buffer is sized at init based on `chunkSize` and
// `maxRelativeRatio`. There is **no** dynamic allocation on the hot path.

#ifndef CLIB_RESAMPLER_ASYNC_POLY_RESAMPLER_H
#define CLIB_RESAMPLER_ASYNC_POLY_RESAMPLER_H

#include <stddef.h>
#include <stdbool.h>
#include "Audio/audio_chunk.h"
#include "resampler_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POLY_INTERPOLATION_LINEAR = 0,
    POLY_INTERPOLATION_CUBIC,
    POLY_INTERPOLATION_QUINTIC,
    POLY_INTERPOLATION_SEPTIC
} poly_interpolation_t;

static inline int poly_interpolation_nbr_points(poly_interpolation_t interp) {
    switch (interp) {
        case POLY_INTERPOLATION_LINEAR: return 2;
        case POLY_INTERPOLATION_CUBIC: return 4;
        case POLY_INTERPOLATION_QUINTIC: return 6;
        case POLY_INTERPOLATION_SEPTIC: return 8;
        default: return 4;
    }
}

typedef struct {
    size_t channels;
    size_t chunk_size;
    poly_interpolation_t interpolation;
    size_t interpolator_len; // = nbr_points
    // Ratio bookkeeping.
    double base_ratio;
    double resample_ratio;
    double target_ratio;
    double last_index; // tracking index
    // Per-channel input buffer. Layout:
    //   [0 .. 2*nbr_points)            — history padding zone
    //   [2*nbr_points .. 2*nbr_points+chunkSize) — current chunk
    audio_buffers_t* input_buffer;
    // Pre-allocated per-frame scratch. `start_idx_scratch` holds the integer
    // floor of `idx`, computed once when `frac_scratch` is built — saving the
    // inner loops a floor() + int cast per output frame.
    int* start_idx_scratch;
    double* frac_scratch;
    size_t max_output_frames;
} async_poly_resampler_t;

async_poly_resampler_t* async_poly_resampler_create(size_t channels, size_t input_rate, size_t output_rate, poly_interpolation_t interpolation, size_t chunk_size, double max_relative_ratio);
void async_poly_resampler_free(async_poly_resampler_t* resampler);

resampler_error_t async_poly_resampler_process(async_poly_resampler_t* resampler, const audio_chunk_t* input, audio_chunk_t* output);
void async_poly_resampler_set_relative_ratio(async_poly_resampler_t* resampler, double multiplier);
double async_poly_resampler_get_ratio(const async_poly_resampler_t* resampler);
size_t async_poly_resampler_get_max_output_frames(const async_poly_resampler_t* resampler);
size_t async_poly_resampler_get_chunk_size(const async_poly_resampler_t* resampler);
size_t async_poly_resampler_get_channels(const async_poly_resampler_t* resampler);

#ifdef __cplusplus
}
#endif

#endif // CLIB_RESAMPLER_ASYNC_POLY_RESAMPLER_H
