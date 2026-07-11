#if defined(ENABLE_ACCELERATE)
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

#include "FFT/complex_inner_real_fft.h"

#include <math.h>
#include <stdlib.h>

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct complex_inner_real_fft {
  real_fft_backend_t base;
  size_t half_n;  // = length / 2 = N
  /// The N-point complex FFT picked at construction. Could be any
  /// `ArbitraryComplexFFT` — `VDSPComplexDFT`, `MixedRadixFFT`, or
  /// `BluesteinFFT` depending on what `RealFFT.init` chose.
  arbitrary_complex_fft_t* inner;
  // Unit-modulus twiddle table `W[k] = exp(-iπk/N)` for k = 0..N-1.
  double* twiddle_re;
  double* twiddle_im;
  // Hot-path scratch (length N).
  double* z_re;
  double* z_im;
  double* z_f_re;
  double* z_f_im;
};

/**
 * @brief Wrapper for the forward FFT implementation.
 *
 * This function conforms to the signature required by the real_fft_backend_t
 * interface. It casts the context pointer back to complex_inner_real_fft_t and
 * calls the actual forward function.
 *
 * @param ctx Pointer to the complex_inner_real_fft_t context.
 * @param real_in Input real waveform.
 * @param spec_re Output real part of the spectrum.
 * @param spec_im Output imaginary part of the spectrum.
 */
static void complex_inner_real_fft_forward_wrapper(void* ctx,
                                                   waveform_t real_in,
                                                   mutable_waveform_t spec_re,
                                                   mutable_waveform_t spec_im) {
  complex_inner_real_fft_forward((complex_inner_real_fft_t*)ctx, real_in,
                                 spec_re, spec_im);
}

/**
 * @brief Wrapper for the inverse FFT implementation.
 *
 * This function conforms to the signature required by the real_fft_backend_t
 * interface. It casts the context pointer back to complex_inner_real_fft_t and
 * calls the actual inverse function.
 *
 * @param ctx Pointer to the complex_inner_real_fft_t context.
 * @param spec_re Input real part of the spectrum.
 * @param spec_im Input imaginary part of the spectrum.
 * @param real_out Output real waveform.
 */
static void complex_inner_real_fft_inverse_wrapper(
    void* ctx, waveform_t spec_re, waveform_t spec_im,
    mutable_waveform_t real_out) {
  complex_inner_real_fft_inverse((complex_inner_real_fft_t*)ctx, spec_re,
                                 spec_im, real_out);
}

/**
 * @brief Wrapper for the free function.
 *
 * This function conforms to the signature required by the real_fft_backend_t
 * interface. It casts the context pointer back to complex_inner_real_fft_t and
 * calls the actual free function.
 *
 * @param ctx Pointer to the complex_inner_real_fft_t context.
 */
static void complex_inner_real_fft_free_wrapper(void* ctx) {
  complex_inner_real_fft_free((complex_inner_real_fft_t*)ctx);
}

complex_inner_real_fft_t* complex_inner_real_fft_create(
    size_t length, arbitrary_complex_fft_t* inner) {
  if (length == 0 || length % 2 != 0 || !inner) return NULL;
  size_t half_n = length / 2;
  complex_inner_real_fft_t* fft =
      (complex_inner_real_fft_t*)calloc(1, sizeof(complex_inner_real_fft_t));
  if (!fft) return NULL;

  fft->base.ctx = fft;
  fft->base.forward = complex_inner_real_fft_forward_wrapper;
  fft->base.inverse = complex_inner_real_fft_inverse_wrapper;
  fft->base.free = complex_inner_real_fft_free_wrapper;
  fft->half_n = half_n;

  fft->twiddle_re = (double*)malloc(half_n * sizeof(double));
  fft->twiddle_im = (double*)malloc(half_n * sizeof(double));
  fft->z_re = (double*)malloc(half_n * sizeof(double));
  fft->z_im = (double*)malloc(half_n * sizeof(double));
  fft->z_f_re = (double*)malloc(half_n * sizeof(double));
  fft->z_f_im = (double*)malloc(half_n * sizeof(double));

  if (!fft->twiddle_re || !fft->twiddle_im || !fft->z_re || !fft->z_im ||
      !fft->z_f_re || !fft->z_f_im) {
    complex_inner_real_fft_free(fft);
    return NULL;
  }

  fft->inner = inner;

  for (size_t k = 0; k < half_n; k++) {
    double theta = -M_PI * (double)k / (double)half_n;
    fft->twiddle_re[k] = cos(theta);
    fft->twiddle_im[k] = sin(theta);
  }
  return fft;
}

void complex_inner_real_fft_forward(complex_inner_real_fft_t* fft,
                                    waveform_t real_in,
                                    mutable_waveform_t spec_re,
                                    mutable_waveform_t spec_im) {
  if (!fft) return;
  size_t n = fft->half_n;
#ifdef ENABLE_ACCELERATE
  // Pack the 2N real samples into N complex: z[k] = x[2k] + i·x[2k+1].
  // Reinterpret `realIn` as interleaved complex pairs and let `vDSP_ctozD`
  // do the deinterleave in one pass.
  DSPDoubleSplitComplex zSplit = {fft->z_re, fft->z_im};
  vDSP_ctozD((const DSPDoubleComplex*)real_in, 2, &zSplit, 1, (vDSP_Length)n);
#else
  for (size_t k = 0; k < n; k++) {
    fft->z_re[k] = real_in[2 * k];
    fft->z_im[k] = real_in[2 * k + 1];
  }
#endif

  // Z = FFT_N(z). Unnormalised forward.
  arbitrary_complex_fft_execute(fft->inner, fft->z_re, fft->z_im, fft->z_f_re,
                                fft->z_f_im, false);

  // DC and Nyquist bins (both real):
  //   X[0] = Re(Z[0]) + Im(Z[0])
  //   X[N] = Re(Z[0]) - Im(Z[0])
  double z0r = fft->z_f_re[0];
  double z0i = fft->z_f_im[0];
  spec_re[0] = z0r + z0i;
  spec_im[0] = 0.0;
  spec_re[n] = z0r - z0i;
  spec_im[n] = 0.0;

  // Generic untwiddle for k ∈ [1, N):
  //   E[k] = ½ · (Z[k] + conj(Z[N-k]))
  //   O[k] = -½·i · (Z[k] - conj(Z[N-k]))
  //   X[k] = E[k] + W^k · O[k],  W^k = exp(-iπk/N)
  //
  // SIMD2 path processes consecutive `k` pairs. The partners (N-k, N-k-1)
  // are also adjacent in memory but in reversed order — we build the
  // SIMD2 explicitly to keep lane 0 = `k` and lane 1 = `k+1`.
  for (size_t k = 1; k < n; k++) {
    double zkR = fft->z_f_re[k];
    double zkI = fft->z_f_im[k];
    double zmR = fft->z_f_re[n - k];
    double zmI = fft->z_f_im[n - k];
    double eRe = 0.5 * (zkR + zmR);
    double eIm = 0.5 * (zkI - zmI);
    double diffRe = zkR - zmR;
    double diffIm = zkI + zmI;
    double oRe = 0.5 * diffIm;
    double oIm = -0.5 * diffRe;
    double twR = fft->twiddle_re[k];
    double twI = fft->twiddle_im[k];
    double woRe = twR * oRe - twI * oIm;
    double woIm = twR * oIm + twI * oRe;
    spec_re[k] = eRe + woRe;
    spec_im[k] = eIm + woIm;
  }
}

void complex_inner_real_fft_inverse(complex_inner_real_fft_t* fft,
                                    waveform_t spec_re, waveform_t spec_im,
                                    mutable_waveform_t real_out) {
  if (!fft) return;
  size_t n = fft->half_n;
  // DC bin packs the special pair (X[0], X[N]):
  //   z[0] = ½·(X[0] + X[N]) + ½·i·(X[0] - X[N])
  double x0 = spec_re[0];
  double xN = spec_re[n];
  fft->z_re[0] = 0.5 * (x0 + xN);
  fft->z_im[0] = 0.5 * (x0 - xN);

  // Generic inverse untwiddle for k ∈ [1, N):
  //   E[k] = ½·(X[k] + conj(X[N-k]))
  //   O[k] = ½·conj(W^k)·(X[k] - conj(X[N-k]))
  //   z[k] = E[k] + i·O[k]
  //
  // SIMD2 path: same partner-mirror trick as in `forward()`.
  for (size_t k = 1; k < n; k++) {
    double xkR = spec_re[k];
    double xkI = spec_im[k];
    double xmR = spec_re[n - k];
    double xmI = spec_im[n - k];
    double eRe = 0.5 * (xkR + xmR);
    double eIm = 0.5 * (xkI - xmI);
    double halfDiffRe = 0.5 * (xkR - xmR);
    double halfDiffIm = 0.5 * (xkI + xmI);
    double twR = fft->twiddle_re[k];
    double twI = fft->twiddle_im[k];
    double oRe = halfDiffRe * twR + halfDiffIm * twI;
    double oIm = halfDiffIm * twR - halfDiffRe * twI;
    fft->z_re[k] = eRe - oIm;
    fft->z_im[k] = eIm + oRe;
  }

  // Inner inverse FFT. The inner returns the unnormalised N-point IFFT,
  // i.e. `N · z`. The textbook unnormalised 2N-point IFFT equals `2 · N · z`,
  // so the unpack picks up a factor of 2.
  arbitrary_complex_fft_execute(fft->inner, fft->z_re, fft->z_im, fft->z_f_re,
                                fft->z_f_im, true);

#ifdef ENABLE_ACCELERATE
  // Scale by 2 in place, then re-interleave back into `realOut` via
  // `vDSP_ztocD`. Two vDSP calls beat the scalar 2N store loop on Apple
  // Silicon when n ≥ ~1k.
  double two = 2.0;
  vDSP_vsmulD(fft->z_f_re, 1, &two, fft->z_f_re, 1, (vDSP_Length)n);
  vDSP_vsmulD(fft->z_f_im, 1, &two, fft->z_f_im, 1, (vDSP_Length)n);
  DSPDoubleSplitComplex zFSplit = {fft->z_f_re, fft->z_f_im};
  vDSP_ztocD(&zFSplit, 1, (DSPDoubleComplex*)real_out, 2, (vDSP_Length)n);
#else
  for (size_t k = 0; k < n; k++) {
    real_out[2 * k] = 2.0 * fft->z_f_re[k];
    real_out[2 * k + 1] = 2.0 * fft->z_f_im[k];
  }
#endif
}

void complex_inner_real_fft_free(complex_inner_real_fft_t* fft) {
  if (!fft) return;
  if (fft->inner) arbitrary_complex_fft_free(fft->inner);
  if (fft->twiddle_re) free(fft->twiddle_re);
  if (fft->twiddle_im) free(fft->twiddle_im);
  if (fft->z_re) free(fft->z_re);
  if (fft->z_im) free(fft->z_im);
  if (fft->z_f_re) free(fft->z_f_re);
  if (fft->z_f_im) free(fft->z_f_im);
  free(fft);
}
#endif  // ENABLE_ACCELERATE
