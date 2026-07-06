#ifndef CLIB_FFT_MIXEDRADIXFFT_H
#define CLIB_FFT_MIXEDRADIXFFT_H

// Arbitrary-N complex DFT via iterative DIT Cooley-Tukey where all prime
// factors are all ≤ 7. Targets `N = 1029 = 3 · 7³` and `N = 1120 = 2⁵ · 5 · 7`
// — the inner FFT sizes that RealFFT needs for 44.1↔48 kHz
// resampling. Compared with Bluestein-on-vDSP, this trades the inner
// power-of-2 transforms (M = 4096) for a direct decomposition into
// `O(N · Σ pᵢ)` ops — about 6× fewer arithmetic operations at N = 1029.
//
// Note on the radix-2/4/8 stages: they're not redundant with
// `RealFFT`'s outer `vDSP_fft_zrip` fast path. That fast path
// fires only when the *whole* real-FFT length is a power of two; the
// radix-2/4/8 stages here handle the *power-of-two portion* of a mixed
// factorisation (e.g. `1120 = 2⁵·5·7` collapses into `[8, 4, 5, 7]`).
// Without them this class could only support odd-prime-only sizes like
// `105 = 3·5·7`, and most of our resampler's mixed-rate FFTs would fall
// through to Bluestein.
//
// Architecture: classic iterative DIT (decimation-in-time) Cooley-Tukey.
//   1. Permute input via mixed-radix digit reversal.
//   2. For each factor `r` (in order), apply length-`r` butterflies on
//      stride-`m` groups, where `m` grows by `r` after each stage. Twiddle
//      factors W_{m·r}^(j·k) are pre-computed once at init.
//   3. Copy out (with conjugation for the inverse direction).
//
// Inverse FFT uses the identity `IDFT(x) = conj(DFT(conj(x)))`, so we only
// pre-compute the forward twiddles. Both transforms are unnormalised.
//
// All buffers (twiddles, permutation LUT, scratch) are heap-allocated at
// init and freed in deinit. The hot path runs purely on raw pointers — no
// allocations, no closures.

#include "arbitrary_complex_fft.h"
#include "Audio/prc_fmt.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Mixed-radix complex FFT supporting `N = 2^a · 3^b · 5^c · 7^d`. Returns
/// `nil` (or NULL in C) if `N` has any prime factor > 7 — caller should fall back to
/// Bluestein in that case.
typedef struct mixed_radix_fft mixed_radix_fft_t;

mixed_radix_fft_t* mixed_radix_fft_create(size_t n);
/// Run the N-point DFT. `inverse=false` is the unnormalised forward
/// transform; `inverse=true` is the unnormalised inverse, so the caller
/// is responsible for any `1/N` normalisation.
void mixed_radix_fft_execute(mixed_radix_fft_t* fft, waveform_t real_in, waveform_t imag_in, mutable_waveform_t real_out, mutable_waveform_t imag_out, bool inverse);
void mixed_radix_fft_free(mixed_radix_fft_t* fft);

static inline arbitrary_complex_fft_t* mixed_radix_fft_as_arbitrary(mixed_radix_fft_t* fft) {
    return (arbitrary_complex_fft_t*)fft;
}

#ifdef __cplusplus
}
#endif

#endif // CLIB_FFT_MIXEDRADIXFFT_H
