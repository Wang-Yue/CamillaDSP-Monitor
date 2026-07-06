#ifndef CLIB_AUDIO_SPECTRUM_ANALYZER_H
#define CLIB_AUDIO_SPECTRUM_ANALYZER_H

#include "Audio/audio_history_buffer.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Result of an FFT spectrum query — bin-center frequencies (Hz) and
/// magnitudes (dBFS).
typedef struct {
    const float* frequencies;
    const float* magnitudes;
    size_t count;
} spectrum_result_t;

/// Status/error codes returned by `spectrum_analyzer_compute(...)`. The spectrum
/// analyzer wraps an `audio_history_buffer_t`; channel-out-of-range errors
/// surface as `SPECTRUM_ERROR_OUT_OF_RANGE` and bubble through unchanged.
typedef enum {
    SPECTRUM_OK = 0,
    /// Not enough samples buffered yet to fill an FFT window.
    SPECTRUM_ERROR_EMPTY = -1,
    /// Caller passed nonsensical FFT parameters.
    SPECTRUM_ERROR_INVALID_PARAM = -2,
    SPECTRUM_ERROR_OUT_OF_RANGE = -3
} spectrum_status_t;

typedef struct {
    int low_k;
    int high_k;
    int nearest_k;
} bin_range_t;

typedef struct {
    double min_freq;
    double max_freq;
    size_t n_bins;
    size_t samplerate;
    float* frequencies;
    bin_range_t* ranges;
    size_t capacity;
} binning_plan_t;

/// Pure spectrum analyzer that operates on an `audio_history_buffer_t`.
typedef struct {
    size_t fft_n;
#ifdef __APPLE__
    vDSP_Length log2n;
    FFTSetup fft_setup;
#else
    void* fft_setup;
    double* fft_in_d;
    double* fft_re_d;
    double* fft_im_d;
#endif
    float* window;
    // Preallocated reusable scratch buffers to eliminate frame-by-frame allocations
    float* data;
    float* realp;
    float* imagp;
    float* magnitudes;
    float* db_magnitudes;

    // Cached plan for geometric binning to eliminate transcendental operations
    binning_plan_t plan;
    float* out_magnitudes;
    size_t out_capacity;
} spectrum_analyzer_t;

spectrum_analyzer_t* spectrum_analyzer_create(void);
void spectrum_analyzer_free(spectrum_analyzer_t* analyzer);

/// Compute a spectrum on demand (consumer side).
spectrum_status_t spectrum_analyzer_compute(
    spectrum_analyzer_t* analyzer,
    audio_history_buffer_t* buffer,
    int channel,
    double min_freq,
    double max_freq,
    size_t n_bins,
    size_t samplerate,
    spectrum_result_t* out_result
);

#ifdef __cplusplus
}
#endif

#endif // CLIB_AUDIO_SPECTRUM_ANALYZER_H
