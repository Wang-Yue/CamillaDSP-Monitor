#ifndef CLIB_FFT_ARBITRARYCOMPLEXFFT_H
#define CLIB_FFT_ARBITRARYCOMPLEXFFT_H

// Shared interface for any complex-input/output DFT engine. The
// `ComplexInnerRealFFT` real-FFT backend takes one of these as its
// inner transform; `RealFFT.init` does the priority-based
// selection between the available conformers.

#include <stdbool.h>

#include "Audio/double_helpers.h"

#ifdef __APPLE__
typedef void (*arbitrary_complex_fft_execute_fn)(void* ctx, waveform_t real_in,
                                                 waveform_t imag_in,
                                                 mutable_waveform_t real_out,
                                                 mutable_waveform_t imag_out,
                                                 bool inverse);
typedef void (*arbitrary_complex_fft_free_fn)(void* ctx);

/// Common interface for any complex-input/output unscaled DFT.
///
/// Conformers in this module:
///   * `BluesteinFFT` — universal fallback for any `n`.
///   * `MixedRadixFFT` — native, supports `n` whose prime factors are
///     in `{2, 3, 5, 7}`.
///   * `VDSPComplexDFT` — Apple's `vDSP_DFT_zopD`, supports
///     `n = f·2ᵐ` with `f ∈ {1, 3, 5, 15}`, `m ≥ 3`.
///
/// All three return the unscaled DFT in both directions (forward
/// followed by inverse scales the input by `n`), so they're
/// interchangeable as `ComplexInnerRealFFT.inner`.
typedef struct {
  void* ctx;
  arbitrary_complex_fft_execute_fn execute;
  arbitrary_complex_fft_free_fn free;
} arbitrary_complex_fft_t;

void arbitrary_complex_fft_execute(arbitrary_complex_fft_t* fft,
                                   waveform_t real_in, waveform_t imag_in,
                                   mutable_waveform_t real_out,
                                   mutable_waveform_t imag_out, bool inverse);
void arbitrary_complex_fft_free(arbitrary_complex_fft_t* fft);
#endif

#endif  // CLIB_FFT_ARBITRARYCOMPLEXFFT_H
