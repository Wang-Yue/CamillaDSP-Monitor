#ifndef CLIB_FFT_VDSPCOMPLEXDFT_H
#define CLIB_FFT_VDSPCOMPLEXDFT_H

/**
 * @file vdsp_complex_dft.h
 * @brief vDSP `DFT_zopD` backend for complex DFTs.
 *
 * This backend is used for complex DFTs at sizes \f$ f \cdot 2^m \f$,
 * where \f$ f \in \{1, 3, 5, 15\} \f$ and \f$ m \ge 3 \f$.
 * It is used by `ComplexInnerRealFFT` as its inner transform when the size
 * qualifies. Apple's tuned mixed-radix is typically faster than
 * `MixedRadixFFT` in this regime.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "arbitrary_complex_fft.h"

#if defined(ENABLE_ACCELERATE)
/**
 * @brief Opaque structure wrapping the vDSP complex DFT state.
 *
 * Wraps `vDSP_DFT_zopD` (complex out-of-place DFT). Setup creation
 * returns `NULL` for any size outside the supported family, in which
 * case the caller (`RealFFT.init`) falls back to
 * `MixedRadixFFT` (small-prime sizes 2/3/5/7) or `BluesteinFFT`
 * (universal).
 *
 * Output convention: unscaled DFT in both directions (round-trip
 * scales the input by `n`), matching `MixedRadixFFT` and
 * `BluesteinFFT` — drop-in for `ComplexInnerRealFFT.inner`.
 */
typedef struct vdsp_complex_dft vdsp_complex_dft_t;

/**
 * @brief Creates a vDSP complex DFT context.
 *
 * @param n The size of the DFT.
 * @return A pointer to the created context, or `NULL` if the size is not
 * supported.
 */
vdsp_complex_dft_t* vdsp_complex_dft_create(size_t n);

/**
 * @brief Executes the complex DFT.
 *
 * @param dft The DFT context.
 * @param real_in Input array for real parts.
 * @param imag_in Input array for imaginary parts.
 * @param real_out Output array for real parts.
 * @param imag_out Output array for imaginary parts.
 * @param inverse `true` for inverse DFT, `false` for forward DFT.
 */
void vdsp_complex_dft_execute(vdsp_complex_dft_t* dft, waveform_t real_in,
                              waveform_t imag_in, mutable_waveform_t real_out,
                              mutable_waveform_t imag_out, bool inverse);

/**
 * @brief Frees the vDSP complex DFT context.
 *
 * @param dft The context to free.
 */
void vdsp_complex_dft_free(vdsp_complex_dft_t* dft);

/**
 * @brief Casts the vDSP complex DFT context to an arbitrary complex FFT
 * interface.
 *
 * @param dft The context to cast.
 * @return A pointer to the arbitrary complex FFT interface.
 */
static inline arbitrary_complex_fft_t* vdsp_complex_dft_as_arbitrary(
    vdsp_complex_dft_t* dft) {
  return (arbitrary_complex_fft_t*)dft;
}
#endif  // ENABLE_ACCELERATE

#endif  // CLIB_FFT_VDSPCOMPLEXDFT_H
