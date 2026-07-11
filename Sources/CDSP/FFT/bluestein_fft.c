#if defined(ENABLE_ACCELERATE)
// Arbitrary-N complex DFT via Bluestein's chirp-z transform.
//
// References:
//   * L. I. Bluestein, "A linear filtering approach to the computation of
//     the discrete Fourier transform," NEREM Record 10, 1968.
//   * L. R. Rabiner, R. W. Schafer, C. M. Rader, "The Chirp z-Transform
//     Algorithm," IEEE Trans. Audio Electroacoust. AU-17(2):86–92, 1969.
//   * Oppenheim & Schafer, *Discrete-Time Signal Processing*, 3rd ed.,
//     §9.6 "Computation of the DFT Using the Chirp Transform Algorithm".
//
// The identity 2nk = n² + k² − (k − n)² rewrites the DFT
//   X[k] = Σₙ x[n]·exp(−2πi·nk/N)
// as the convolution
//   X[k] = exp(−iπk²/N) · Σₙ (x[n]·exp(−iπn²/N)) · exp(+iπ(k−n)²/N).
// The inner sum is the convolution of the chirp-modulated input with the
// length-(2N−1) chirp kernel b[n] = exp(+iπn²/N). We zero-pad both to the
// smallest power of two M ≥ 2N − 1 and evaluate the convolution via the
// standard FFT-multiply-IFFT pipeline; the outer chirp is applied as a
// pointwise post-multiply.
//
// vDSP's complex FFT (`vDSP_DFT_zop_CreateSetupD`) only accepts power-of-2
// lengths ≥ 16, so logical sizes that aren't powers of two (e.g. the
// L+M block lengths chosen by `SynchronousResampler`) need this fallback.
// Cost is three length-M FFTs per logical N-point transform, still
// O(N log N).
//
// Storage uses raw `UnsafeMutablePointer<Double>` buffers (allocated in init,
// freed in deinit) so the hot path can hand them straight to vDSP without
// nested `withUnsafe*` closures. All complex multiplications run through
// `vDSP_zvmulD`, which on Apple Silicon issues packed NEON `fmla.2d` pairs.

#include "FFT/bluestein_fft.h"

#include <Accelerate/Accelerate.h>
#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct bluestein_fft {
  arbitrary_complex_fft_t base;
  /// Logical DFT length.
  size_t n;
  /// Inner power-of-2 FFT length, ≥ 2n − 1 and ≥ 16 (vDSP's minimum).
  size_t m;
  // Forward chirp `α[k] = exp(-iπk²/N)`, length n. Stored as
  // (cos(πk²/N), -sin(πk²/N)).
  double* alpha_re;
  double* alpha_im;
  // Same chirp pre-scaled by 1/m, used in the post-multiply step. Folding
  // the IFFT's missing 1/m scale into α here lets us skip two
  // length-m `vDSP_vsmulD` calls per execute() call.
  double* alpha_post_re;
  double* alpha_post_im;
  // Pre-FFT'd b sequence (length m), used in the convolution step.
  double* b_real_f;
  double* b_imag_f;
  vDSP_DFT_SetupD fft_fwd;
  vDSP_DFT_SetupD fft_inv;
  // Hot-path scratch (length m).
  double* a_re;
  double* a_im;
  double* a_re_f;
  double* a_im_f;
  double* p_re;
  double* p_im;
  double* c_re;
  double* c_im;
};

/**
 * @brief Static wrapper to execute the Bluestein FFT, matching the
 * arbitrary_complex_fft interface.
 *
 * @param ctx The generic context pointer (pointing to a bluestein_fft_t
 * instance).
 * @param real_in Input real component array.
 * @param imag_in Input imaginary component array.
 * @param real_out Output real component array.
 * @param imag_out Output imaginary component array.
 * @param inverse True for inverse DFT, false for forward DFT.
 */
static void bluestein_fft_execute_wrapper(void* ctx, waveform_t real_in,
                                          waveform_t imag_in,
                                          mutable_waveform_t real_out,
                                          mutable_waveform_t imag_out,
                                          bool inverse) {
  bluestein_fft_execute((bluestein_fft_t*)ctx, real_in, imag_in, real_out,
                        imag_out, inverse);
}

/**
 * @brief Static wrapper to free the Bluestein FFT context, matching the
 * arbitrary_complex_fft interface.
 *
 * @param ctx The generic context pointer (pointing to a bluestein_fft_t
 * instance).
 */
static void bluestein_fft_free_wrapper(void* ctx) {
  bluestein_fft_free((bluestein_fft_t*)ctx);
}

bluestein_fft_t* bluestein_fft_create(size_t n, config_error_t* err) {
  if (n == 0) {
    config_error_set(err, CONFIG_ERR_PARSE, "BluesteinFFT: n must be positive");
    return NULL;
  }

  // Find the smallest optimal size `m` for the inner FFT.
  // vDSP optimized sizes are of the form f * 2^k, where f in {1, 3, 5, 15} and
  // k >= 3 (or 4 for f=1). The size must be at least 2n - 1 to prevent
  // time-domain aliasing during the linear convolution.
  size_t min_l = 2 * n - 1;
  size_t best_m = (size_t)-1;
  int factors[4] = {1, 3, 5, 15};
  for (int i = 0; i < 4; i++) {
    int f = factors[i];
    int min_k = (f == 1) ? 4 : 3;
    double target = (double)min_l / (double)f;
    int k = min_k;
    while ((1 << k) < target) k++;
    size_t m_val = (size_t)f * (1 << k);
    if (m_val < best_m) best_m = m_val;
  }
  size_t m = best_m;

  vDSP_DFT_SetupD fwd =
      vDSP_DFT_zop_CreateSetupD(NULL, (vDSP_Length)m, vDSP_DFT_FORWARD);
  if (!fwd) {
    config_error_set(err, CONFIG_ERR_PARSE, "BluesteinFFT: vDSP forward DFT setup failed for inner size %zu", m);
    return NULL;
  }
  vDSP_DFT_SetupD inv =
      vDSP_DFT_zop_CreateSetupD(NULL, (vDSP_Length)m, vDSP_DFT_INVERSE);
  if (!inv) {
    config_error_set(err, CONFIG_ERR_PARSE, "BluesteinFFT: vDSP inverse DFT setup failed for inner size %zu", m);
    vDSP_DFT_DestroySetupD(fwd);
    return NULL;
  }

  bluestein_fft_t* fft = (bluestein_fft_t*)calloc(1, sizeof(bluestein_fft_t));
  if (!fft) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate BluesteinFFT");
    vDSP_DFT_DestroySetupD(fwd);
    vDSP_DFT_DestroySetupD(inv);
    return NULL;
  }
  fft->base.ctx = fft;
  fft->base.execute = bluestein_fft_execute_wrapper;
  fft->base.free = bluestein_fft_free_wrapper;
  fft->n = n;
  fft->m = m;
  fft->fft_fwd = fwd;
  fft->fft_inv = inv;

  fft->alpha_re = (double*)calloc(n, sizeof(double));
  fft->alpha_im = (double*)calloc(n, sizeof(double));
  fft->alpha_post_re = (double*)calloc(n, sizeof(double));
  fft->alpha_post_im = (double*)calloc(n, sizeof(double));
  fft->b_real_f = (double*)calloc(m, sizeof(double));
  fft->b_imag_f = (double*)calloc(m, sizeof(double));
  fft->a_re = (double*)calloc(m, sizeof(double));
  fft->a_im = (double*)calloc(m, sizeof(double));
  fft->a_re_f = (double*)calloc(m, sizeof(double));
  fft->a_im_f = (double*)calloc(m, sizeof(double));
  fft->p_re = (double*)calloc(m, sizeof(double));
  fft->p_im = (double*)calloc(m, sizeof(double));
  fft->c_re = (double*)calloc(m, sizeof(double));
  fft->c_im = (double*)calloc(m, sizeof(double));

  if (!fft->alpha_re || !fft->alpha_im || !fft->alpha_post_re ||
      !fft->alpha_post_im || !fft->b_real_f || !fft->b_imag_f || !fft->a_re ||
      !fft->a_im || !fft->a_re_f || !fft->a_im_f || !fft->p_re || !fft->p_im ||
      !fft->c_re || !fft->c_im) {
    bluestein_fft_free(fft);
    return NULL;
  }

  double inv_md = 1.0 / (double)m;
  // Initialise the outer chirp α[k] = exp(-iπk²/N) (Rabiner-Schafer-Rader
  // 1969, eq. 8). The `(k*k) % (2*n)` reduction keeps the trig argument
  // bounded so cos/sin retain full precision for large N. `alphaPost`
  // stores the same chirp scaled by 1/M to absorb the IFFT normalisation
  // into the post-multiply step.
  for (size_t k = 0; k < n; k++) {
    double theta = M_PI * (double)((k * k) % (2 * n)) / (double)n;
    double c = cos(theta);
    double s = -sin(theta);
    fft->alpha_re[k] = c;
    fft->alpha_im[k] = s;
    fft->alpha_post_re[k] = c * inv_md;
    fft->alpha_post_im[k] = s * inv_md;
  }

  double* b_re = (double*)calloc(m, sizeof(double));
  double* b_im = (double*)calloc(m, sizeof(double));
  if (!b_re || !b_im) {
    free(b_re);
    free(b_im);
    bluestein_fft_free(fft);
    return NULL;
  }
  if (b_re && b_im) {
    // Build the chirp kernel b[k] = exp(+iπk²/N), zero-padded and
    // periodically extended to length M so that the M-point cyclic
    // convolution computes the desired linear convolution over the
    // valid range k ∈ [0, n). Per Oppenheim & Schafer §9.6:
    //   b[0]   = 1
    //   b[k]   = exp(+iπk²/N)            for k = 1..n-1
    //   b[m-k] = b[k]                    (mirrored copy at the wrap)
    //   b[k]   = 0                       elsewhere
    // We FFT this kernel once at setup; the hot path multiplies the
    // input's spectrum by it pointwise.
    b_re[0] = 1.0;
    for (size_t k = 1; k < n; k++) {
      double theta = M_PI * (double)((k * k) % (2 * n)) / (double)n;
      double c = cos(theta);
      double s = sin(theta);
      b_re[k] = c;
      b_im[k] = s;
      b_re[m - k] = c;
      b_im[m - k] = s;
    }
    vDSP_DFT_ExecuteD(fwd, b_re, b_im, fft->b_real_f, fft->b_imag_f);
  }
  free(b_re);
  free(b_im);

  return fft;
}

void bluestein_fft_execute(bluestein_fft_t* fft, waveform_t real_in,
                           waveform_t imag_in, mutable_waveform_t real_out,
                           mutable_waveform_t imag_out, bool inverse) {
  if (!fft) return;
  size_t n = fft->n;
  size_t m = fft->m;
  // Step 1: a[0..n) = α · x (forward) or α · conj(x) (inverse).
  // Tried `vDSP_zvmulD` here too — it benchmarks slower than this scalar
  // loop because the compiler already vectorises the simple form and the
  // vDSP per-call setup dominates for n ≈ 2k. Keeping scalar.
  double conj_sign = inverse ? -1.0 : 1.0;
  for (size_t k = 0; k < n; k++) {
    double xr = real_in[k];
    double xi = imag_in[k] * conj_sign;
    double ar = fft->alpha_re[k];
    double ai = fft->alpha_im[k];
    fft->a_re[k] = xr * ar - xi * ai;
    fft->a_im[k] = xr * ai + xi * ar;
  }
  if (m > n) {
    // Zero-pad the rest of `a` up to length m.
    for (size_t k = n; k < m; k++) {
      fft->a_re[k] = 0.0;
      fft->a_im[k] = 0.0;
    }
  }

  // Step 2: cyclic convolution via FFT — A = FFT(a); P = A · B;
  // c = IFFT(P) / m.
  // We perform the convolution of the modulated input (a) and the chirp kernel
  // (b) in the frequency domain. `b_real_f` and `b_imag_f` contain the
  // pre-computed FFT of b.
  vDSP_DFT_ExecuteD(fft->fft_fwd, fft->a_re, fft->a_im, fft->a_re_f,
                    fft->a_im_f);
  DSPDoubleSplitComplex aFSplit = {fft->a_re_f, fft->a_im_f};
  DSPDoubleSplitComplex bSplit = {fft->b_real_f, fft->b_imag_f};
  DSPDoubleSplitComplex pSplit = {fft->p_re, fft->p_im};
  vDSP_zvmulD(&aFSplit, 1, &bSplit, 1, &pSplit, 1, (vDSP_Length)m, 1);
  vDSP_DFT_ExecuteD(fft->fft_inv, fft->p_re, fft->p_im, fft->c_re, fft->c_im);
  // The IFFT's missing `1/m` scale is folded into `alphaPost`, so no
  // separate vDSP_vsmulD is needed here.

  // Step 3: post-multiply, write to caller's output.
  //   forward: out = α' · c           (α' = α/m)
  //   inverse: out = conj(α' · c) — negate imag after the regular product.
  // We apply the outer chirp multiplication to obtain the final DFT result.
  DSPDoubleSplitComplex alphaPostSplit = {fft->alpha_post_re,
                                          fft->alpha_post_im};
  DSPDoubleSplitComplex cSplit = {fft->c_re, fft->c_im};
  DSPDoubleSplitComplex outSplit = {real_out, imag_out};
  vDSP_zvmulD(&alphaPostSplit, 1, &cSplit, 1, &outSplit, 1, (vDSP_Length)n, 1);
  if (inverse) {
    vDSP_vnegD(imag_out, 1, imag_out, 1, (vDSP_Length)n);
  }
}

void bluestein_fft_free(bluestein_fft_t* fft) {
  if (!fft) return;
  if (fft->fft_fwd) vDSP_DFT_DestroySetupD(fft->fft_fwd);
  if (fft->fft_inv) vDSP_DFT_DestroySetupD(fft->fft_inv);
  free(fft->alpha_re);
  free(fft->alpha_im);
  free(fft->alpha_post_re);
  free(fft->alpha_post_im);
  free(fft->b_real_f);
  free(fft->b_imag_f);
  free(fft->a_re);
  free(fft->a_im);
  free(fft->a_re_f);
  free(fft->a_im_f);
  free(fft->p_re);
  free(fft->p_im);
  free(fft->c_re);
  free(fft->c_im);
  free(fft);
}
#endif  // ENABLE_ACCELERATE
