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

#include "FFT/mixed_radix_fft.h"

#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct mixed_radix_fft {
  arbitrary_complex_fft_t base;
  size_t n;
  int stage_count;
  /// Prime factorisation of `n`, smallest first. The DIT stages walk this
  /// list left-to-right.
  int* factors;
  /// Per-stage forward twiddles, length `m_s · r_s` (for stage `s` with
  /// pre-stage subblock size `m_s` and radix `r_s`). The `j = 0` row is
  /// trivial (W^0 = 1) but we keep it for uniform indexing.
  double** twiddle_re;
  double** twiddle_im;
  /// Mixed-radix digit-reversal permutation. `permutation[i]` is where
  /// input element `i` ends up in the post-permutation buffer.
  int* permutation;
  /// Active read/write buffers for the butterfly stages. Re-pointed at
  /// the caller's `realOut`/`imagOut` at the start of each `execute`
  /// call — the permutation step writes the post-permute samples
  /// directly into the output buffer, every stage runs in-place on
  /// the output, and we skip the final memcpy that the older "internal
  /// scratch + copy out" pattern needed.
  ///
  /// Only valid for the duration of one `execute` invocation. Aliasing
  /// `realIn` and `realOut` is unsupported (the permute pass would
  /// overwrite input bytes mid-pass).
  double* work_re;
  double* work_im;
};

/**
 * @brief Wrapper for the mixed-radix FFT execution.
 *
 * Conforms to the arbitrary_complex_fft_t interface.
 */
static void mixed_radix_fft_execute_wrapper(void* ctx, waveform_t real_in,
                                            waveform_t imag_in,
                                            mutable_waveform_t real_out,
                                            mutable_waveform_t imag_out,
                                            bool inverse) {
  mixed_radix_fft_execute((mixed_radix_fft_t*)ctx, real_in, imag_in, real_out,
                          imag_out, inverse);
}

/**
 * @brief Wrapper for the mixed-radix FFT free function.
 *
 * Conforms to the arbitrary_complex_fft_t interface.
 */
static void mixed_radix_fft_free_wrapper(void* ctx) {
  mixed_radix_fft_free((mixed_radix_fft_t*)ctx);
}

/**
 * @brief Apply radix-2 butterflies across `n / (m·2)` blocks of size `m·2`.
 *
 * Twiddle table layout: twRe[j·m + k] for j ∈ {0, 1}, k ∈ [0, m).
 *
 * Computes:
 *   v0 = x0 + x1 * tw
 *   v1 = x0 - x1 * tw
 *
 * @param fft Pointer to the FFT context.
 * @param m Current subblock size.
 * @param tw_re Real part of twiddle factors.
 * @param tw_im Imaginary part of twiddle factors.
 */
static inline void stage_radix2(mixed_radix_fft_t* fft, int m,
                                const double* tw_re, const double* tw_im) {
  int block_size = m * 2;
  double* work_re = fft->work_re;
  double* work_im = fft->work_im;
  for (int b = 0; b < (int)fft->n; b += block_size) {
    for (int k = 0; k < m; k++) {
      int i0 = b + k;
      int i1 = i0 + m;
      double twR = tw_re[m + k];
      double twI = tw_im[m + k];
      double v1r = work_re[i1] * twR - work_im[i1] * twI;
      double v1i = work_re[i1] * twI + work_im[i1] * twR;
      double v0r = work_re[i0];
      double v0i = work_im[i0];
      work_re[i0] = v0r + v1r;
      work_im[i0] = v0i + v1i;
      work_re[i1] = v0r - v1r;
      work_im[i1] = v0i - v1i;
    }
  }
}

/**
 * @brief Apply radix-3 butterflies. Same layout as radix-2.
 *
 * Uses the standard radix-3 DFT formulation:
 *   s = v1 + v2
 *   d = v1 - v2
 *   a = v0 - 0.5 * s
 *   b = i * sin(2π/3) * d
 *   O[0] = v0 + s
 *   O[1] = a + b
 *   O[2] = a - b
 *
 * @param fft Pointer to the FFT context.
 * @param m Current subblock size.
 * @param tw_re Real part of twiddle factors.
 * @param tw_im Imaginary part of twiddle factors.
 */
static inline void stage_radix3(mixed_radix_fft_t* fft, int m,
                                const double* tw_re, const double* tw_im) {
  int block_size = m * 3;
  double* work_re = fft->work_re;
  double* work_im = fft->work_im;
  // W3 = exp(-2π i / 3) = (-1/2, -√3/2). The constant `√3/2` recurs below.
  double s32 = sin(2.0 * M_PI / 3.0);
  for (int b = 0; b < (int)fft->n; b += block_size) {
    for (int k = 0; k < m; k++) {
      int i0 = b + k;
      int i1 = i0 + m;
      int i2 = i1 + m;
      double tw1R = tw_re[m + k];
      double tw1I = tw_im[m + k];
      double tw2R = tw_re[2 * m + k];
      double tw2I = tw_im[2 * m + k];
      // Twiddle.
      double v1r = work_re[i1] * tw1R - work_im[i1] * tw1I;
      double v1i = work_re[i1] * tw1I + work_im[i1] * tw1R;
      double v2r = work_re[i2] * tw2R - work_im[i2] * tw2I;
      double v2i = work_re[i2] * tw2I + work_im[i2] * tw2R;
      double v0r = work_re[i0];
      double v0i = work_im[i0];
      // Radix-3 DFT.
      double sR = v1r + v2r;
      double sI = v1i + v2i;
      double dR = v1r - v2r;
      double dI = v1i - v2i;
      double aR = v0r - 0.5 * sR;
      double aI = v0i - 0.5 * sI;
      double bR = s32 * dR;
      double bI = s32 * dI;
      work_re[i0] = v0r + sR;
      work_im[i0] = v0i + sI;
      work_re[i1] = aR + bI;
      work_im[i1] = aI - bR;
      work_re[i2] = aR - bI;
      work_im[i2] = aI + bR;
    }
  }
}

/**
 * @brief Apply radix-4 butterflies.
 *
 * The inner DFT is multiplication-free — the four 4th-roots of unity are
 * `{1, -i, -1, i}`, so the inner stage is just adds and ±i swaps.
 * Only the 3 outer-stage twiddles (`v[1], v[2], v[3]`) cost real multiplies.
 *
 * @param fft Pointer to the FFT context.
 * @param m Current subblock size.
 * @param tw_re Real part of twiddle factors.
 * @param tw_im Imaginary part of twiddle factors.
 */
static inline void stage_radix4(mixed_radix_fft_t* fft, int m,
                                const double* tw_re, const double* tw_im) {
  int block_size = m * 4;
  double* work_re = fft->work_re;
  double* work_im = fft->work_im;
  for (int b = 0; b < (int)fft->n; b += block_size) {
    for (int k = 0; k < m; k++) {
      int i0 = b + k;
      int i1 = i0 + m;
      int i2 = i1 + m;
      int i3 = i2 + m;
      double t1R = tw_re[m + k];
      double t1I = tw_im[m + k];
      double t2R = tw_re[2 * m + k];
      double t2I = tw_im[2 * m + k];
      double t3R = tw_re[3 * m + k];
      double t3I = tw_im[3 * m + k];
      double v0r = work_re[i0];
      double v0i = work_im[i0];
      double v1r = work_re[i1] * t1R - work_im[i1] * t1I;
      double v1i = work_re[i1] * t1I + work_im[i1] * t1R;
      double v2r = work_re[i2] * t2R - work_im[i2] * t2I;
      double v2i = work_re[i2] * t2I + work_im[i2] * t2R;
      double v3r = work_re[i3] * t3R - work_im[i3] * t3I;
      double v3i = work_re[i3] * t3I + work_im[i3] * t3R;
      // Inner radix-4 DFT: T0=v0+v2, T1=v0-v2, T2=v1+v3, T3=v1-v3
      // O[0]=T0+T2, O[1]=T1-i·T3, O[2]=T0-T2, O[3]=T1+i·T3
      // -i·z = (z.im, -z.re); +i·z = (-z.im, z.re).
      double t0r = v0r + v2r;
      double t0i = v0i + v2i;
      double t1r2 = v0r - v2r;
      double t1i2 = v0i - v2i;
      double t2r2 = v1r + v3r;
      double t2i2 = v1i + v3i;
      double t3r2 = v1r - v3r;
      double t3i2 = v1i - v3i;
      work_re[i0] = t0r + t2r2;
      work_im[i0] = t0i + t2i2;
      work_re[i1] = t1r2 + t3i2;
      work_im[i1] = t1i2 - t3r2;
      work_re[i2] = t0r - t2r2;
      work_im[i2] = t0i - t2i2;
      work_re[i3] = t1r2 - t3i2;
      work_im[i3] = t1i2 + t3r2;
    }
  }
}

/**
 * @brief Apply radix-5 butterflies.
 *
 * Output layout uses the conjugate-pair factoring trick:
 *     O[k] = v0 + Σ_p W^(p·k) · v_p,  W = exp(-2πi/5)
 *
 * Outputs `O[1]` and `O[4]` differ only in the sign of the `W^(p·k)·v_p`
 * imaginary parts (since `W^4 = conj(W)`); same for `O[2]` and `O[3]`
 * (since `W^3 = conj(W²)`). Pre-compute a "common" w_R-weighted sum and
 * a "twist" w_I-weighted sum once per pair, then assemble the four
 * outputs as `r0 ± common ± twist`. Cuts the multiplies per butterfly
 * from ~48 to ~32 (-33 %) without changing the scalar tail's
 * arithmetic identity.
 *
 * @param fft Pointer to the FFT context.
 * @param m Current subblock size.
 * @param tw_re Real part of twiddle factors.
 * @param tw_im Imaginary part of twiddle factors.
 */
static inline void stage_radix5(mixed_radix_fft_t* fft, int m,
                                const double* tw_re, const double* tw_im) {
  int block_size = m * 5;
  double* work_re = fft->work_re;
  double* work_im = fft->work_im;
  // Radix-5 uses these inner DFT constants. tw_5^k = exp(-2πi·k/5).
  double w1R = cos(2.0 * M_PI / 5.0);
  double w1I = -sin(2.0 * M_PI / 5.0);
  double w2R = cos(4.0 * M_PI / 5.0);
  double w2I = -sin(4.0 * M_PI / 5.0);
  for (int b = 0; b < (int)fft->n; b += block_size) {
    for (int k = 0; k < m; k++) {
      int i0 = b + k;
      int i1 = i0 + m;
      int i2 = i1 + m;
      int i3 = i2 + m;
      int i4 = i3 + m;
      // Outer-stage twiddle on samples 1..4.
      double t1R = tw_re[m + k];
      double t1I = tw_im[m + k];
      double t2R = tw_re[2 * m + k];
      double t2I = tw_im[2 * m + k];
      double t3R = tw_re[3 * m + k];
      double t3I = tw_im[3 * m + k];
      double t4R = tw_re[4 * m + k];
      double t4I = tw_im[4 * m + k];
      double v1r = work_re[i1] * t1R - work_im[i1] * t1I;
      double v1i = work_re[i1] * t1I + work_im[i1] * t1R;
      double v2r = work_re[i2] * t2R - work_im[i2] * t2I;
      double v2i = work_re[i2] * t2I + work_im[i2] * t2R;
      double v3r = work_re[i3] * t3R - work_im[i3] * t3I;
      double v3i = work_re[i3] * t3I + work_im[i3] * t3R;
      double v4r = work_re[i4] * t4R - work_im[i4] * t4I;
      double v4i = work_re[i4] * t4I + work_im[i4] * t4R;
      double v0r = work_re[i0];
      double v0i = work_im[i0];
      // Radix-5 DFT (direct, not Winograd). 4 unique inner products plus
      // the DC term — straightforward and lets the compiler issue plenty
      // of FMAs.
      //
      //   O[0] = v0 + v1 + v2 + v3 + v4
      //   O[k] = v0 + W^k·v1 + W^(2k)·v2 + W^(3k)·v3 + W^(4k)·v4,
      //          W = exp(-2πi/5).
      //
      // Since W^4 = conj(W) and W^3 = conj(W²), the four non-DC outputs
      // come in two conjugate pairs: (O[1], O[4]) and (O[2], O[3]). Each
      // pair shares a "common" (w_R · sum) term and a "twist"
      // (w_I · diff) term — see the SIMD2 body above for the derivation.
      double sum14R = v1r + v4r;
      double sum14I = v1i + v4i;
      double diff14R = v1r - v4r;
      double diff14I = v1i - v4i;
      double sum23R = v2r + v3r;
      double sum23I = v2i + v3i;
      double diff23R = v2r - v3r;
      double diff23I = v2i - v3i;
      // O[0]
      work_re[i0] = v0r + sum14R + sum23R;
      work_im[i0] = v0i + sum14I + sum23I;
      // Conjugate pair (1, 4).
      double cR14 = w1R * sum14R + w2R * sum23R;
      double cI14 = w1R * sum14I + w2R * sum23I;
      double tR14 = w1I * diff14I + w2I * diff23I;
      double tI14 = w1I * diff14R + w2I * diff23R;
      work_re[i1] = v0r + cR14 - tR14;
      work_im[i1] = v0i + cI14 + tI14;
      work_re[i4] = v0r + cR14 + tR14;
      work_im[i4] = v0i + cI14 - tI14;
      // Conjugate pair (2, 3).
      double cR23 = w2R * sum14R + w1R * sum23R;
      double cI23 = w2R * sum14I + w1R * sum23I;
      double tR23 = w2I * diff14I - w1I * diff23I;
      double tI23 = w2I * diff14R - w1I * diff23R;
      work_re[i2] = v0r + cR23 - tR23;
      work_im[i2] = v0i + cI23 + tI23;
      work_re[i3] = v0r + cR23 + tR23;
      work_im[i3] = v0i + cI23 - tI23;
    }
  }
}

/**
 * @brief Apply radix-7 butterflies.
 *
 * Direct DFT — 6 unique pairs of conjugate twiddles. Compute each output as
 * `v0 + Σ pair-products`.
 *
 * The six non-DC outputs come in three conjugate pairs:
 * `(O[1], O[6])`, `(O[2], O[5])`, `(O[3], O[4])` — each pair shares a
 * w_R-weighted "common" term and a w_I-weighted "twist" term, with
 * only the twist's sign (and the imag flip) distinguishing the two
 * outputs in a pair. This factoring cuts the multiplies per butterfly
 * from ~96 to ~60 (-38 %) versus computing each output from scratch.
 *
 * @param fft Pointer to the FFT context.
 * @param m Current subblock size.
 * @param tw_re Real part of twiddle factors.
 * @param tw_im Imaginary part of twiddle factors.
 */
static inline void stage_radix7(mixed_radix_fft_t* fft, int m,
                                const double* tw_re, const double* tw_im) {
  int block_size = m * 7;
  double* work_re = fft->work_re;
  double* work_im = fft->work_im;
  double w1R = cos(2.0 * M_PI / 7.0);
  double w1I = -sin(2.0 * M_PI / 7.0);
  double w2R = cos(4.0 * M_PI / 7.0);
  double w2I = -sin(4.0 * M_PI / 7.0);
  double w3R = cos(6.0 * M_PI / 7.0);
  double w3I = -sin(6.0 * M_PI / 7.0);
  // Cache the m-multiples once per stage call so the inner loop
  // computes each twiddle base address with one `add` instead of
  // a `mul` per iteration.
  int m2 = m << 1;
  int m3 = m2 + m;
  int m4 = m << 2;
  int m5 = m4 + m;
  int m6 = m3 << 1;
  for (int b = 0; b < (int)fft->n; b += block_size) {
    for (int k = 0; k < m; k++) {
      int i0 = b + k;
      int i1 = i0 + m;
      int i2 = i1 + m;
      int i3 = i2 + m;
      int i4 = i3 + m;
      int i5 = i4 + m;
      int i6 = i5 + m;
      // Outer-stage twiddles on samples 1..6. Reuse the cached
      // `m2..m6` from above so the twiddle base addresses are
      // single `+` ops instead of `Int` multiplies with overflow
      // traps.
      double t1R = tw_re[m + k];
      double t1I = tw_im[m + k];
      double t2R = tw_re[m2 + k];
      double t2I = tw_im[m2 + k];
      double t3R = tw_re[m3 + k];
      double t3I = tw_im[m3 + k];
      double t4R = tw_re[m4 + k];
      double t4I = tw_im[m4 + k];
      double t5R = tw_re[m5 + k];
      double t5I = tw_im[m5 + k];
      double t6R = tw_re[m6 + k];
      double t6I = tw_im[m6 + k];
      double v1r = work_re[i1] * t1R - work_im[i1] * t1I;
      double v1i = work_re[i1] * t1I + work_im[i1] * t1R;
      double v2r = work_re[i2] * t2R - work_im[i2] * t2I;
      double v2i = work_re[i2] * t2I + work_im[i2] * t2R;
      double v3r = work_re[i3] * t3R - work_im[i3] * t3I;
      double v3i = work_re[i3] * t3I + work_im[i3] * t3R;
      double v4r = work_re[i4] * t4R - work_im[i4] * t4I;
      double v4i = work_re[i4] * t4I + work_im[i4] * t4R;
      double v5r = work_re[i5] * t5R - work_im[i5] * t5I;
      double v5i = work_re[i5] * t5I + work_im[i5] * t5R;
      double v6r = work_re[i6] * t6R - work_im[i6] * t6I;
      double v6i = work_re[i6] * t6I + work_im[i6] * t6R;
      double v0r = work_re[i0];
      double v0i = work_im[i0];
      // Build pair sums/diffs with conjugate-symmetric partners.
      // {1,6}: W^1, W^6 = W^-1 → coef pair (w1, conj(w1))
      // {2,5}: W^2, W^5 = W^-2 → (w2, conj(w2))
      // {3,4}: W^3, W^4 = W^-3 → (w3, conj(w3))
      double s16R = v1r + v6r;
      double s16I = v1i + v6i;
      double d16R = v1r - v6r;
      double d16I = v1i - v6i;
      double s25R = v2r + v5r;
      double s25I = v2i + v5i;
      double d25R = v2r - v5r;
      double d25I = v2i - v5i;
      double s34R = v3r + v4r;
      double s34I = v3i + v4i;
      double d34R = v3r - v4r;
      double d34I = v3i - v4i;
      // O[0] = v0 + sum of all sums.
      work_re[i0] = v0r + s16R + s25R + s34R;
      work_im[i0] = v0i + s16I + s25I + s34I;
      // Conjugate-pair factoring.
      // Pair (1, 6).
      double cR16 = w1R * s16R + w2R * s25R + w3R * s34R;
      double cI16 = w1R * s16I + w2R * s25I + w3R * s34I;
      double tR16 = w1I * d16I + w2I * d25I + w3I * d34I;
      double tI16 = w1I * d16R + w2I * d25R + w3I * d34R;
      work_re[i1] = v0r + cR16 - tR16;
      work_im[i1] = v0i + cI16 + tI16;
      work_re[i6] = v0r + cR16 + tR16;
      work_im[i6] = v0i + cI16 - tI16;
      // Pair (2, 5).
      double cR25 = w2R * s16R + w3R * s25R + w1R * s34R;
      double cI25 = w2R * s16I + w3R * s25I + w1R * s34I;
      double tR25 = w2I * d16I - w3I * d25I - w1I * d34I;
      double tI25 = w2I * d16R - w3I * d25R - w1I * d34R;
      work_re[i2] = v0r + cR25 - tR25;
      work_im[i2] = v0i + cI25 + tI25;
      work_re[i5] = v0r + cR25 + tR25;
      work_im[i5] = v0i + cI25 - tI25;
      // Pair (3, 4).
      double cR34 = w3R * s16R + w1R * s25R + w2R * s34R;
      double cI34 = w3R * s16I + w1R * s25I + w2R * s34I;
      double tR34 = w3I * d16I - w1I * d25I + w2I * d34I;
      double tI34 = w3I * d16R - w1I * d25R + w2I * d34R;
      work_re[i3] = v0r + cR34 - tR34;
      work_im[i3] = v0i + cI34 + tI34;
      work_re[i4] = v0r + cR34 + tR34;
      work_im[i4] = v0i + cI34 - tI34;
    }
  }
}

/**
 * @brief Apply radix-8 butterflies.
 *
 * The inner DFT is computed via DIT decomposition into two radix-4s
 * (even-indexed and odd-indexed), then combined with the trivial 8th-root
 * twiddles `W_8^k = exp(-2πi·k/8)`. Multiplications cost only the constant
 * `√2/2` for the k=1 and k=3 inner twiddles — k=0 is free, k=2 is
 * `-i` (free), so no real-coefficient multiplies on the inner DFT
 * beyond the two `√2/2` cross-terms.
 *
 * @param fft Pointer to the FFT context.
 * @param m Current subblock size.
 * @param tw_re Real part of twiddle factors.
 * @param tw_im Imaginary part of twiddle factors.
 */
static inline void stage_radix8(mixed_radix_fft_t* fft, int m,
                                const double* tw_re, const double* tw_im) {
  int block_size = m * 8;
  double* work_re = fft->work_re;
  double* work_im = fft->work_im;
  double s2 = 0.7071067811865476;  // √2/2
  for (int b = 0; b < (int)fft->n; b += block_size) {
    for (int k = 0; k < m; k++) {
      int i0 = b + k;
      int i1 = i0 + m;
      int i2 = i1 + m;
      int i3 = i2 + m;
      int i4 = i3 + m;
      int i5 = i4 + m;
      int i6 = i5 + m;
      int i7 = i6 + m;
      double t1R = tw_re[m + k];
      double t1I = tw_im[m + k];
      double t2R = tw_re[2 * m + k];
      double t2I = tw_im[2 * m + k];
      double t3R = tw_re[3 * m + k];
      double t3I = tw_im[3 * m + k];
      double t4R = tw_re[4 * m + k];
      double t4I = tw_im[4 * m + k];
      double t5R = tw_re[5 * m + k];
      double t5I = tw_im[5 * m + k];
      double t6R = tw_re[6 * m + k];
      double t6I = tw_im[6 * m + k];
      double t7R = tw_re[7 * m + k];
      double t7I = tw_im[7 * m + k];
      double v0r = work_re[i0];
      double v0i = work_im[i0];
      double v1r = work_re[i1] * t1R - work_im[i1] * t1I;
      double v1i = work_re[i1] * t1I + work_im[i1] * t1R;
      double v2r = work_re[i2] * t2R - work_im[i2] * t2I;
      double v2i = work_re[i2] * t2I + work_im[i2] * t2R;
      double v3r = work_re[i3] * t3R - work_im[i3] * t3I;
      double v3i = work_re[i3] * t3I + work_im[i3] * t3R;
      double v4r = work_re[i4] * t4R - work_im[i4] * t4I;
      double v4i = work_re[i4] * t4I + work_im[i4] * t4R;
      double v5r = work_re[i5] * t5R - work_im[i5] * t5I;
      double v5i = work_re[i5] * t5I + work_im[i5] * t5R;
      double v6r = work_re[i6] * t6R - work_im[i6] * t6I;
      double v6i = work_re[i6] * t6I + work_im[i6] * t6R;
      double v7r = work_re[i7] * t7R - work_im[i7] * t7I;
      double v7i = work_re[i7] * t7I + work_im[i7] * t7R;
      // Even radix-4: DFT of (v0, v2, v4, v6).
      double eA0r = v0r + v4r;
      double eA0i = v0i + v4i;
      double eA1r = v0r - v4r;
      double eA1i = v0i - v4i;
      double eA2r = v2r + v6r;
      double eA2i = v2i + v6i;
      double eA3r = v2r - v6r;
      double eA3i = v2i - v6i;
      double e0r = eA0r + eA2r;
      double e0i = eA0i + eA2i;
      double e1r = eA1r + eA3i;
      double e1i = eA1i - eA3r;
      double e2r = eA0r - eA2r;
      double e2i = eA0i - eA2i;
      double e3r = eA1r - eA3i;
      double e3i = eA1i + eA3r;
      // Odd radix-4: DFT of (v1, v3, v5, v7).
      double oA0r = v1r + v5r;
      double oA0i = v1i + v5i;
      double oA1r = v1r - v5r;
      double oA1i = v1i - v5i;
      double oA2r = v3r + v7r;
      double oA2i = v3i + v7i;
      double oA3r = v3r - v7r;
      double oA3i = v3i - v7i;
      double oo0r = oA0r + oA2r;
      double oo0i = oA0i + oA2i;
      double oo1r = oA1r + oA3i;
      double oo1i = oA1i - oA3r;
      double oo2r = oA0r - oA2r;
      double oo2i = oA0i - oA2i;
      double oo3r = oA1r - oA3i;
      double oo3i = oA1i + oA3r;
      // Apply W_8^k to odd outputs:
      //   W_8^0 = 1; W_8^1 = (s2, -s2); W_8^2 = -i; W_8^3 = (-s2, -s2).
      double w0r = oo0r;
      double w0i = oo0i;
      double w1r = s2 * (oo1r + oo1i);
      double w1i = s2 * (oo1i - oo1r);
      double w2r = oo2i;
      double w2i = -oo2r;
      double w3r = s2 * (oo3i - oo3r);
      double w3i = -s2 * (oo3r + oo3i);
      // O[k] = E[k] + W_8^k·O_odd[k], O[k+4] = E[k] - W_8^k·O_odd[k].
      work_re[i0] = e0r + w0r;
      work_im[i0] = e0i + w0i;
      work_re[i1] = e1r + w1r;
      work_im[i1] = e1i + w1i;
      work_re[i2] = e2r + w2r;
      work_im[i2] = e2i + w2i;
      work_re[i3] = e3r + w3r;
      work_im[i3] = e3i + w3i;
      work_re[i4] = e0r - w0r;
      work_im[i4] = e0i - w0i;
      work_re[i5] = e1r - w1r;
      work_im[i5] = e1i - w1i;
      work_re[i6] = e2r - w2r;
      work_im[i6] = e2i - w2i;
      work_re[i7] = e3r - w3r;
      work_im[i7] = e3i - w3i;
    }
  }
}

mixed_radix_fft_t* mixed_radix_fft_create(size_t n) {
  if (n == 0) return NULL;
  // Factorise into 2/3/4/5/7/8 with the power-of-2 portion preferring
  // larger radixes — `2⁵ = 32 → [8, 4]` (2 stages) vs `[2, 2, 2, 2, 2]`
  // (5 stages). Each stage saved cuts a length-N twiddle multiply pass
  // and the loop-overhead that comes with it. For N = 1120 = 2⁵·5·7 this
  // collapses 7 stages to 4: `[8, 4, 5, 7]`.
  int fs[32];
  int stage_count = 0;
  size_t rem = n;
  int two_pow = 0;
  while (rem % 2 == 0) {
    two_pow++;
    rem /= 2;
  }
  // Greedy: take 8s while we have ≥ 3 powers of 2 remaining, then a
  // single 4 if 2 remain, or a 2 if 1 remains.
  while (two_pow >= 3) {
    fs[stage_count++] = 8;
    two_pow -= 3;
  }
  if (two_pow == 2) {
    fs[stage_count++] = 4;
  } else if (two_pow == 1) {
    fs[stage_count++] = 2;
  }
  int primes[3] = {3, 5, 7};
  for (int i = 0; i < 3; i++) {
    int p = primes[i];
    while (rem % (size_t)p == 0) {
      fs[stage_count++] = p;
      rem /= (size_t)p;
    }
  }
  if (rem != 1) return NULL;  // unsupported large prime

  mixed_radix_fft_t* fft =
      (mixed_radix_fft_t*)calloc(1, sizeof(mixed_radix_fft_t));
  if (!fft) return NULL;

  fft->base.ctx = fft;
  fft->base.execute = mixed_radix_fft_execute_wrapper;
  fft->base.free = mixed_radix_fft_free_wrapper;
  fft->n = n;
  fft->stage_count = stage_count;
  fft->factors = (int*)calloc(stage_count, sizeof(int));
  fft->twiddle_re = (double**)calloc(stage_count, sizeof(double*));
  fft->twiddle_im = (double**)calloc(stage_count, sizeof(double*));
  fft->permutation = (int*)calloc(n, sizeof(int));

  if (!fft->factors || !fft->twiddle_re || !fft->twiddle_im ||
      !fft->permutation) {
    mixed_radix_fft_free(fft);
    return NULL;
  }

  for (int s = 0; s < stage_count; s++) {
    fft->factors[s] = fs[s];
    fft->twiddle_re[s] = NULL;
    fft->twiddle_im[s] = NULL;
  }

  // Allocate per-stage twiddle buffers.
  int m = 1;
  for (int s = 0; s < stage_count; s++) {
    int r = fs[s];
    int len = m * r;
    fft->twiddle_re[s] = (double*)malloc(len * sizeof(double));
    fft->twiddle_im[s] = (double*)malloc(len * sizeof(double));
    if (!fft->twiddle_re[s] || !fft->twiddle_im[s]) {
      mixed_radix_fft_free(fft);
      return NULL;
    }
    // twiddle[j*m + k] = W_{m·r}^(j·k) for j in 0..r-1, k in 0..m-1.
    double invMR = 1.0 / (double)(m * r);
    for (int j = 0; j < r; j++) {
      for (int k = 0; k < m; k++) {
        double theta = -2.0 * M_PI * (double)(j * k) * invMR;
        fft->twiddle_re[s][j * m + k] = cos(theta);
        fft->twiddle_im[s][j * m + k] = sin(theta);
      }
    }
    m *= r;
  }

  // Pre-compute the digit-reversal permutation. We store `factors` in
  // stage-iteration order (`factors[0]` is the radix processed first, with
  // `m = 1`); the corresponding decimation order, used to build the perm,
  // is the reverse. So we iterate `factors.reversed()` here. Failing to
  // reverse leaves stage 0 operating on the wrong input groups — the bug
  // that turned this whole mixed-radix path into garbage on the first
  // attempt.
  for (size_t i = 0; i < n; i++) {
    size_t idx = i;
    size_t rev = 0;
    size_t m_left = n;
    for (int s = stage_count - 1; s >= 0; s--) {
      int r = fs[s];
      m_left /= (size_t)r;
      size_t d = idx % (size_t)r;
      idx /= (size_t)r;
      rev += d * m_left;
    }
    fft->permutation[i] = (int)rev;
  }
  // No internal work buffer: `execute` re-points `workRe`/`workIm`
  // at the caller's output for the duration of the call.

  return fft;
}

void mixed_radix_fft_execute(mixed_radix_fft_t* fft, waveform_t real_in,
                             waveform_t imag_in, mutable_waveform_t real_out,
                             mutable_waveform_t imag_out, bool inverse) {
  if (!fft) return;
  // Aim the stage methods at the caller's output. The permute pass
  // writes there, every butterfly stage runs in-place on it, and the
  // result is already in the right place when we're done — no final
  // memcpy. (Allocating a separate work buffer + copying out doubles
  // the memory traffic for what's already a memory-bound kernel.)
  fft->work_re = real_out;
  fft->work_im = imag_out;

  // Step 1: permute input. For inverse, conjugate as we go
  // (DFT(x*) = (DFT(x))*, so we conjugate input then conjugate the
  // final output to flip the transform direction).
  if (inverse) {
    for (size_t i = 0; i < fft->n; i++) {
      int p = fft->permutation[i];
      fft->work_re[p] = real_in[i];
      fft->work_im[p] = -imag_in[i];
    }
  } else {
    for (size_t i = 0; i < fft->n; i++) {
      int p = fft->permutation[i];
      fft->work_re[p] = real_in[i];
      fft->work_im[p] = imag_in[i];
    }
  }

  // Step 2: butterfly stages, all in-place on (workRe, workIm) =
  // (realOut, imagOut).
  int m = 1;
  for (int s = 0; s < fft->stage_count; s++) {
    int r = fft->factors[s];
    const double* twRe = fft->twiddle_re[s];
    const double* twIm = fft->twiddle_im[s];
    switch (r) {
      case 2:
        stage_radix2(fft, m, twRe, twIm);
        break;
      case 3:
        stage_radix3(fft, m, twRe, twIm);
        break;
      case 4:
        stage_radix4(fft, m, twRe, twIm);
        break;
      case 5:
        stage_radix5(fft, m, twRe, twIm);
        break;
      case 7:
        stage_radix7(fft, m, twRe, twIm);
        break;
      case 8:
        stage_radix8(fft, m, twRe, twIm);
        break;
      default:
        break;
    }
    m *= r;
  }

  // Step 3: re-conjugate the imaginary part for the inverse direction.
  // Forward direction is already done in place — no copy needed.
  if (inverse) {
    for (size_t i = 0; i < fft->n; i++) {
      imag_out[i] = -imag_out[i];
    }
  }
}

void mixed_radix_fft_free(mixed_radix_fft_t* fft) {
  if (!fft) return;
  if (fft->twiddle_re) {
    for (int s = 0; s < fft->stage_count; s++) {
      if (fft->twiddle_re[s]) free(fft->twiddle_re[s]);
    }
    free(fft->twiddle_re);
  }
  if (fft->twiddle_im) {
    for (int s = 0; s < fft->stage_count; s++) {
      if (fft->twiddle_im[s]) free(fft->twiddle_im[s]);
    }
    free(fft->twiddle_im);
  }
  if (fft->factors) free(fft->factors);
  if (fft->permutation) free(fft->permutation);
  free(fft);
}
