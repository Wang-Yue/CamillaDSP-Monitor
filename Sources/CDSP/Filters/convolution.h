#ifndef CLIB_FILTERS_CONVOLUTION_H
#define CLIB_FILTERS_CONVOLUTION_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/filter_config_types.h"
#include "FFT/real_fft.h"

#ifdef __cplusplus
extern "C" {
#endif

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
typedef struct {
  char name[64];
  /// Block length `N` (one input chunk per `process` call).
  size_t chunk_size;
  /// Number of `chunkSize`-long IR segments.
  size_t num_segments;
  /// Unique-bin count `N + 1` / FFT length `2N`.
  real_fft_t* fft;
  /// Pre-FFT'd IR segments and rolling input-spectrum history. Each is a
  /// flat `nsegments * bins` block of `PrcFmt`; the per-segment slice for
  /// segment `s` lives at `[s * bins ..< (s + 1) * bins]`.
  double** spec_re;
  double** spec_im;
  double** hist_re;
  double** hist_im;
  /// Index of the input-history slot most recently filled (mod `nsegments`).
  size_t write_idx;
  /// Overlap-save state, length `N` — the second half of the previous
  /// IFFT result, summed into the next block's first half.
  double* overlap_buffer;
  // Time-domain scratch buffers, both `2N` long.
  double* time_buf;
  /// Per-call accumulator for `Σ_seg input_F[hist] · coeffs_F[seg]`.
  double* spec_accum_re;
  double* spec_accum_im;
} convolution_filter_t;

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

#ifdef __cplusplus
}
#endif

#endif  // CLIB_FILTERS_CONVOLUTION_H
