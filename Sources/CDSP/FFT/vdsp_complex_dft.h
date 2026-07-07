#ifndef CLIB_FFT_VDSPCOMPLEXDFT_H
#define CLIB_FFT_VDSPCOMPLEXDFT_H

// vDSP `DFT_zopD` backend for complex DFTs at sizes `f·2ᵐ`,
// `f ∈ {1, 3, 5, 15}`, `m ≥ 3`. Used by `ComplexInnerRealFFT` as its
// inner transform when the size qualifies — Apple's tuned mixed-radix
// is typically faster than `MixedRadixFFT` in this regime.

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "arbitrary_complex_fft.h"

/// Wraps `vDSP_DFT_zopD` (complex out-of-place DFT). Setup creation
/// returns `nil` for any size outside the supported family, in which
/// case the caller (`RealFFT.init`) falls back to
/// `MixedRadixFFT` (small-prime sizes 2/3/5/7) or `BluesteinFFT`
/// (universal).
///
/// Output convention: unscaled DFT in both directions (round-trip
/// scales the input by `n`), matching `MixedRadixFFT` and
/// `BluesteinFFT` — drop-in for `ComplexInnerRealFFT.inner`.
#if defined(ENABLE_ACCELERATE)
typedef struct vdsp_complex_dft vdsp_complex_dft_t;

vdsp_complex_dft_t* vdsp_complex_dft_create(size_t n);
void vdsp_complex_dft_execute(vdsp_complex_dft_t* dft, waveform_t real_in,
                              waveform_t imag_in, mutable_waveform_t real_out,
                              mutable_waveform_t imag_out, bool inverse);
void vdsp_complex_dft_free(vdsp_complex_dft_t* dft);

static inline arbitrary_complex_fft_t* vdsp_complex_dft_as_arbitrary(
    vdsp_complex_dft_t* dft) {
  return (arbitrary_complex_fft_t*)dft;
}
#endif  // ENABLE_ACCELERATE

#endif  // CLIB_FFT_VDSPCOMPLEXDFT_H
