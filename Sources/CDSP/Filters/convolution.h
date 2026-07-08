#ifndef CLIB_FILTERS_CONVOLUTION_H
#define CLIB_FILTERS_CONVOLUTION_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/filter_config_types.h"


// Uniform-partitioned overlap-save FIR convolution.
// Stockham-style segmented overlap-save with one 2N-point real FFT per
// chunk and an N+1-bin spectrum-domain multiply-accumulate across the
// segment history.
//
//   - Uses `RealFFT`, which stores the same N+1 unique bins as separate
//     `specRe`/`specIm` arrays. The flat layout (DC at index 0, Nyquist
//     at index N, both with `im == 0`) lets us run the spectrum
//     multiply through `vDSP_zvmulD` / `vDSP_zvmaD` without any DC/
//     Nyquist special-casing.
//   - `RealFFT.inverse` produces `length · signal`. The inverse does not
//     scale, so we pre-divide coefficients by
//     `2 * data_length` to compensate.
//   - All hot-path buffers are owned by raw `UnsafeMutablePointer`s
//     (`AudioBuffers`-style) so `process(waveform:)` cannot trip
//     Swift's Array CoW path that a `[PrcFmt]` field would.

/// Source format for the impulse response. Parameters:
///
///   - `.values`: inline IR samples in `values`.
///   - `.wav`:    `filename` (24/16/32f/64f WAV), single channel `channel`.
///   - `.raw`:    `filename` of a flat sample stream, one of FLOAT64,
///                FLOAT32, S32_LE, S16_LE, or TEXT (newline-separated).
///   - `.dummy`:  generates a Kronecker delta of length `length`. Used
///                for sanity-checks; the filter becomes a pure delay.
///
/// Coefficient file readers. Off the audio thread — straightforward
/// `Data`-based parsers, no streaming or memory-mapping.
typedef struct convolution_filter convolution_filter_t;

/// Build a convolution filter from raw IR samples.
///
/// - Parameters:
///   - coefficients: Impulse response, in time-domain sample order.
///     Must be non-empty.
///   - chunkSize: Per-call block length `N`. Must match the
///     `validFrames` the pipeline will hand to `process`.
///
/// Resolve the parameters to a flat IR buffer. Only called from the
/// control plane (filter creation / hot-swap), never from
/// `process(waveform:)`.
///
/// Convenience initialiser that resolves `ConvParameters` to a flat
/// IR buffer first (control plane only, may touch the filesystem).
convolution_filter_t* convolution_filter_create(const char* name,
                                                const conv_parameters_t* params,
                                                size_t chunk_size);

/// Process one block in-place. The hot path is allocation-free in
/// steady state; everything below is pointer arithmetic over the
/// preallocated storage from `init`.
void convolution_filter_process(convolution_filter_t* filter,
                                mutable_waveform_t waveform, size_t count);
void convolution_filter_free(convolution_filter_t* filter);

#endif  // CLIB_FILTERS_CONVOLUTION_H
