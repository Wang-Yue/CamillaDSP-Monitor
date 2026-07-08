#ifndef CLIB_FFT_ARBITRARYCOMPLEXFFT_H
#define CLIB_FFT_ARBITRARYCOMPLEXFFT_H

/**
 * @file arbitrary_complex_fft.h
 * @brief Shared interface for complex-input/output Discrete Fourier Transform
 * (DFT) engines.
 *
 * The `ComplexInnerRealFFT` real-FFT backend takes one of these as its
 * inner transform; `RealFFT.init` does the priority-based
 * selection between the available conformers.
 */

#include <stdbool.h>

#include "Audio/double_helpers.h"

/**
 * @brief Function pointer type for executing a complex FFT.
 *
 * @param ctx Pointer to the FFT implementation context.
 * @param real_in Input array representing the real part of the complex input.
 * @param imag_in Input array representing the imaginary part of the complex
 * input.
 * @param real_out Output array to store the real part of the complex output.
 * @param imag_out Output array to store the imaginary part of the complex
 * output.
 * @param inverse True to perform an inverse FFT (IFFT), false for a forward
 * FFT.
 */
typedef void (*arbitrary_complex_fft_execute_fn)(void* ctx, waveform_t real_in,
                                                 waveform_t imag_in,
                                                 mutable_waveform_t real_out,
                                                 mutable_waveform_t imag_out,
                                                 bool inverse);

/**
 * @brief Function pointer type for freeing the FFT implementation context.
 *
 * @param ctx Pointer to the FFT implementation context to free.
 */
typedef void (*arbitrary_complex_fft_free_fn)(void* ctx);

/**
 * @brief Common interface for any complex-input/output unscaled DFT.
 *
 * Conformers in this module:
 *   * `BluesteinFFT` — universal fallback for any `n`.
 *   * `MixedRadixFFT` — native, supports `n` whose prime factors are
 *     in `{2, 3, 5, 7}`.
 *   * `VDSPComplexDFT` — Apple's `vDSP_DFT_zopD`, supports
 *     `n = f·2ᵐ` with `f ∈ {1, 3, 5, 15}`, `m ≥ 3`.
 *
 * All three return the unscaled DFT in both directions (forward
 * followed by inverse scales the input by `n`), so they're
 * interchangeable as `ComplexInnerRealFFT.inner`.
 */
typedef struct {
  void* ctx; /**< Implementation-specific context pointer. */
  arbitrary_complex_fft_execute_fn
      execute; /**< Function pointer to execute the FFT. */
  arbitrary_complex_fft_free_fn
      free; /**< Function pointer to free the context. */
} arbitrary_complex_fft_t;

/**
 * @brief Executes the complex FFT using the underlying implementation.
 *
 * @param fft Pointer to the FFT instance.
 * @param real_in Input array representing the real part.
 * @param imag_in Input array representing the imaginary part.
 * @param real_out Output array to store the real part.
 * @param imag_out Output array to store the imaginary part.
 * @param inverse True for inverse FFT, false for forward FFT.
 */
void arbitrary_complex_fft_execute(arbitrary_complex_fft_t* fft,
                                   waveform_t real_in, waveform_t imag_in,
                                   mutable_waveform_t real_out,
                                   mutable_waveform_t imag_out, bool inverse);

/**
 * @brief Frees the FFT instance and its internal context.
 *
 * @param fft Pointer to the FFT instance to free.
 */
void arbitrary_complex_fft_free(arbitrary_complex_fft_t* fft);

#endif  // CLIB_FFT_ARBITRARYCOMPLEXFFT_H
