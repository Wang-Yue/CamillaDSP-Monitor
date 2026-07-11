#ifndef CLIB_FILTERS_CONVOLUTION_H
#define CLIB_FILTERS_CONVOLUTION_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/filter_config_types.h"

/**
 * @file convolution.h
 * @brief Uniform-partitioned overlap-save FIR convolution filter.
 *
 * Stockham-style segmented overlap-save with one 2N-point real FFT per
 * chunk and an N+1-bin spectrum-domain multiply-accumulate across the
 * segment history.
 *
 * - Uses RealFFT, which stores the same N+1 unique bins as separate
 *   specRe/specIm arrays. The flat layout (DC at index 0, Nyquist
 *   at index N, both with im == 0) lets us run the spectrum
 *   multiply through vDSP_zvmulD / vDSP_zvmaD without any DC/
 *   Nyquist special-casing.
 * - RealFFT.inverse produces length * signal. The inverse does not
 *   scale, so we pre-divide coefficients by 2 * data_length to compensate.
 * - All hot-path buffers are owned by raw UnsafeMutablePointers
 *   (AudioBuffers-style) so process(waveform:) cannot trip
 *   Swift's Array CoW path that a [PrcFmt] field would.
 */

/**
 * @brief Opaque handle to a convolution filter instance.
 */
typedef struct convolution_filter convolution_filter_t;

/**
 * @brief Build a convolution filter from raw IR samples.
 *
 * Resolve the parameters to a flat IR buffer. Only called from the
 * control plane (filter creation / hot-swap), never from
 * convolution_filter_process.
 *
 * @param name The name of the filter.
 * @param params The convolution parameters specifying the impulse response.
 *               Supported formats in params:
 *               - values: inline IR samples in values.
 *               - wav:    filename (24/16/32f/64f WAV), single channel channel.
 *               - raw:    filename of a flat sample stream, one of FLOAT64,
 *                         FLOAT32, S32_LE, S16_LE, or TEXT (newline-separated).
 *               - dummy:  generates a Kronecker delta of length length. Used
 *                         for sanity-checks; the filter becomes a pure delay.
 * @param chunk_size Per-call block length N. Must match the
 *                   validFrames the pipeline will hand to process.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created convolution filter, or NULL on failure.
 */
convolution_filter_t* convolution_filter_create(const char* name,
                                                const conv_parameters_t* params,
                                                size_t chunk_size,
                                                config_error_t* err);

/**
 * @brief Process one block in-place.
 *
 * The hot path is allocation-free in steady state; everything below is
 * pointer arithmetic over the preallocated storage from creation.
 *
 * @param filter The convolution filter instance.
 * @param waveform The input/output waveform buffer.
 * @param count The number of samples to process.
 */
void convolution_filter_process(convolution_filter_t* filter,
                                mutable_waveform_t waveform, size_t count);

/**
 * @brief Free the convolution filter instance and its associated resources.
 *
 * @param filter The convolution filter instance to free.
 */
void convolution_filter_free(convolution_filter_t* filter);

#endif  // CLIB_FILTERS_CONVOLUTION_H
