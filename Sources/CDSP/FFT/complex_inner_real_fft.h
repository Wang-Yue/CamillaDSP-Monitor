#ifndef CLIB_FFT_COMPLEXINNERREALFFT_H
#define CLIB_FFT_COMPLEXINNERREALFFT_H

// Real-FFT backend that builds a 2N-point real FFT from one N-point
// complex FFT plus an O(N) "untwiddle" pass. Used for any even length
// that doesn't qualify for `VDSPRealFFT` (i.e. non-power-of-two, or
// pow2 < 8).
//
// The inner N-point complex FFT is supplied by the caller —
// `RealFFT.init` picks between `VDSPComplexDFT`,
// `MixedRadixFFT`, and `BluesteinFFT` based on `halfN`'s factorisation.
// This class stays purely about the real-FFT structure (packing,
// untwiddle, inverse unpack) and never re-decides the backend.
//
// Algorithm references:
//   - https://www.dsprelated.com/showarticle/4.php (Real FFT from complex FFT)
//   - https://en.wikipedia.org/wiki/Fast_Fourier_transform#Real-input_FFTs

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "arbitrary_complex_fft.h"
#include "real_fft.h"

/// Computes a 2N-point real FFT via an N-point complex FFT plus an O(N)
/// untwiddle. The inner complex FFT is supplied by the caller —
/// `RealFFT.init` does the priority-based selection so this
/// class stays purely about the real-FFT structure (packing, untwiddle,
/// inverse unpack) and never re-decides the backend.
#if defined(ENABLE_ACCELERATE)
typedef struct complex_inner_real_fft complex_inner_real_fft_t;

complex_inner_real_fft_t* complex_inner_real_fft_create(
    size_t length, arbitrary_complex_fft_t* inner);
void complex_inner_real_fft_forward(complex_inner_real_fft_t* fft,
                                    waveform_t real_in,
                                    mutable_waveform_t spec_re,
                                    mutable_waveform_t spec_im);
void complex_inner_real_fft_inverse(complex_inner_real_fft_t* fft,
                                    waveform_t spec_re, waveform_t spec_im,
                                    mutable_waveform_t real_out);
void complex_inner_real_fft_free(complex_inner_real_fft_t* fft);

static inline real_fft_backend_t* complex_inner_real_fft_as_backend(
    complex_inner_real_fft_t* fft) {
  return (real_fft_backend_t*)fft;
}
#endif  // ENABLE_ACCELERATE

#endif  // CLIB_FFT_COMPLEXINNERREALFFT_H
