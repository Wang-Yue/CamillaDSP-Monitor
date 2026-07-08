#ifndef CLIB_FFT_MIXEDRADIXFFT_H
#define CLIB_FFT_MIXEDRADIXFFT_H

/**
 * @file mixed_radix_fft.h
 * @brief Arbitrary-N complex DFT via iterative Cooley-Tukey mixed-radix FFT.
 *
 * Arbitrary-N complex DFT via iterative DIT Cooley-Tukey where all prime
 * factors are all ≤ 7. Targets `N = 1029 = 3 · 7³` and `N = 1120 = 2⁵ · 5 · 7`
 * — the inner FFT sizes that RealFFT needs for 44.1↔48 kHz
 * resampling. Compared with Bluestein-on-vDSP, this trades the inner
 * power-of-2 transforms (M = 4096) for a direct decomposition into
 * `O(N · Σ pᵢ)` ops — about 6× fewer arithmetic operations at N = 1029.
 *
 * Note on the radix-2/4/8 stages: they're not redundant with
 * `RealFFT`'s outer `vDSP_fft_zrip` fast path. That fast path
 * fires only when the *whole* real-FFT length is a power of two; the
 * radix-2/4/8 stages here handle the *power-of-two portion* of a mixed
 * factorisation (e.g. `1120 = 2⁵·5·7` collapses into `[8, 4, 5, 7]`).
 * Without them this class could only support odd-prime-only sizes like
 * `105 = 3·5·7`, and most of our resampler's mixed-rate FFTs would fall
 * through to Bluestein.
 *
 * Architecture:
 *   1. Permute input via mixed-radix digit reversal.
 *   2. For each factor `r` (in order), apply length-`r` butterflies on
 *      stride-`m` groups, where `m` grows by `r` after each stage. Twiddle
 *      factors W_{m·r}^(j·k) are pre-computed once at init.
 *   3. Copy out (with conjugation for the inverse direction).
 *
 * Inverse FFT uses the identity `IDFT(x) = conj(DFT(conj(x)))`, so we only
 * pre-compute the forward twiddles. Both transforms are unnormalised.
 *
 * All buffers (twiddles, permutation LUT, scratch) are heap-allocated at
 * init and freed in deinit. The hot path runs purely on raw pointers.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "arbitrary_complex_fft.h"

/**
 * @struct mixed_radix_fft
 * @brief Opaque structure representing a mixed-radix complex FFT context.
 */
typedef struct mixed_radix_fft mixed_radix_fft_t;

/**
 * @brief Creates a mixed-radix complex FFT context for length N.
 *
 * Mixed-radix complex FFT supporting `N = 2^a · 3^b · 5^c · 7^d`.
 *
 * @param n The transform length.
 * @return A pointer to the created mixed_radix_fft_t context, or NULL if
 *         `n` has any prime factor > 7.
 */
mixed_radix_fft_t* mixed_radix_fft_create(size_t n);

/**
 * @brief Computes the N-point complex DFT.
 *
 * @param fft The mixed-radix FFT context.
 * @param real_in Input buffer for the real parts of the signal.
 * @param imag_in Input buffer for the imaginary parts of the signal.
 * @param real_out Output buffer for the real parts of the result.
 * @param imag_out Output buffer for the imaginary parts of the result.
 * @param inverse false for unnormalised forward transform, true for
 * unnormalised inverse.
 */
void mixed_radix_fft_execute(mixed_radix_fft_t* fft, waveform_t real_in,
                             waveform_t imag_in, mutable_waveform_t real_out,
                             mutable_waveform_t imag_out, bool inverse);

/**
 * @brief Frees the mixed-radix FFT context.
 *
 * @param fft The context to destroy.
 */
void mixed_radix_fft_free(mixed_radix_fft_t* fft);

/**
 * @brief Casts the mixed-radix FFT context to a generic arbitrary complex FFT
 * context.
 *
 * @param fft The mixed-radix FFT context.
 * @return A pointer to the arbitrary_complex_fft_t context.
 */
static inline arbitrary_complex_fft_t* mixed_radix_fft_as_arbitrary(
    mixed_radix_fft_t* fft) {
  return (arbitrary_complex_fft_t*)fft;
}

#endif  // CLIB_FFT_MIXEDRADIXFFT_H
