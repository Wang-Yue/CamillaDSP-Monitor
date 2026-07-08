#ifndef CLIB_FFT_COMPLEXINNERREALFFT_H
#define CLIB_FFT_COMPLEXINNERREALFFT_H

/**
 * @file complex_inner_real_fft.h
 * @brief Real-FFT backend that builds a 2N-point real FFT from one N-point
 * complex FFT.
 *
 * Real-FFT backend that builds a 2N-point real FFT from one N-point
 * complex FFT plus an O(N) "untwiddle" pass. Used for any even length
 * that doesn't qualify for `VDSPRealFFT` (i.e. non-power-of-two, or
 * pow2 < 8).
 *
 * The inner N-point complex FFT is supplied by the caller —
 * `RealFFT.init` picks between `VDSPComplexDFT`,
 * `MixedRadixFFT`, and `BluesteinFFT` based on `halfN`'s factorisation.
 * This class stays purely about the real-FFT structure (packing,
 * untwiddle, inverse unpack) and never re-decides the backend.
 *
 * Algorithm references:
 *   - https://www.dsprelated.com/showarticle/4.php (Real FFT from complex FFT)
 *   - https://en.wikipedia.org/wiki/Fast_Fourier_transform#Real-input_FFTs
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "arbitrary_complex_fft.h"
#include "real_fft.h"

#if defined(ENABLE_ACCELERATE)
/**
 * @struct complex_inner_real_fft
 * @brief Opaque structure representing a Complex Inner Real FFT context.
 */
typedef struct complex_inner_real_fft complex_inner_real_fft_t;

/**
 * @brief Creates a Complex Inner Real FFT context.
 *
 * @param length The time-domain length of the real FFT (must be even, length =
 * 2N).
 * @param inner The N-point complex FFT backend to use internally.
 * @return A pointer to the created context, or NULL on failure.
 */
complex_inner_real_fft_t* complex_inner_real_fft_create(
    size_t length, arbitrary_complex_fft_t* inner);

/**
 * @brief Computes the forward 2N-point real FFT.
 *
 * Computes a 2N-point real FFT via an N-point complex FFT plus an O(N)
 * untwiddle. Produces N+1 unique complex bins.
 *
 * @param fft The Complex Inner Real FFT context.
 * @param real_in Input buffer of real samples (length must be >= length).
 * @param spec_re Output buffer for the real parts of the spectrum (length >=
 * length/2 + 1).
 * @param spec_im Output buffer for the imaginary parts of the spectrum (length
 * >= length/2 + 1).
 */
void complex_inner_real_fft_forward(complex_inner_real_fft_t* fft,
                                    waveform_t real_in,
                                    mutable_waveform_t spec_re,
                                    mutable_waveform_t spec_im);

/**
 * @brief Computes the inverse 2N-point real FFT.
 *
 * @param fft The Complex Inner Real FFT context.
 * @param spec_re Input buffer containing the real parts of the spectrum.
 * @param spec_im Input buffer containing the imaginary parts of the spectrum.
 * @param real_out Output buffer for the reconstructed real samples.
 */
void complex_inner_real_fft_inverse(complex_inner_real_fft_t* fft,
                                    waveform_t spec_re, waveform_t spec_im,
                                    mutable_waveform_t real_out);

/**
 * @brief Frees the Complex Inner Real FFT context.
 *
 * @param fft The context to destroy.
 */
void complex_inner_real_fft_free(complex_inner_real_fft_t* fft);

/**
 * @brief Casts the context to a generic real FFT backend.
 *
 * @param fft The Complex Inner Real FFT context.
 * @return A pointer to the real_fft_backend_t context.
 */
static inline real_fft_backend_t* complex_inner_real_fft_as_backend(
    complex_inner_real_fft_t* fft) {
  return (real_fft_backend_t*)fft;
}
#endif  // ENABLE_ACCELERATE

#endif  // CLIB_FFT_COMPLEXINNERREALFFT_H
