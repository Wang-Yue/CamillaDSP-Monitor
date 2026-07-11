#include "FFT/real_fft.h"

#include <stdlib.h>
#include <string.h>

struct real_fft {
  size_t length;          /**< Time-domain length (must be even). */
  size_t spectrum_length; /**< Number of unique complex bins in the spectrum (=
                             length/2 + 1). */
  real_fft_backend_t* backend; /**< The dispatched backend implementation. */
};

size_t real_fft_get_length(const real_fft_t* fft) {
  return fft ? fft->length : 0;
}

size_t real_fft_get_spectrum_length(const real_fft_t* fft) {
  return fft ? fft->spectrum_length : 0;
}

#if defined(ENABLE_ACCELERATE)

// Real-input FFT of arbitrary even length. `RealFFT.init` is
// the **single dispatch point** for the resampler's FFT subsystem — it
// inspects the requested length once and picks the fastest available
// backend, so callers (and the per-backend classes) never repeat that
// decision.
//
// Decision tree (top-to-bottom, first match wins)
// ------------------------------------------------
//   1. `length` is a power of two `≥ 8`
//      → `VDSPRealFFT` (`VDSPRealFFT.swift`), wrapping Apple's
//      `vDSP_fft_zrip` / `vDSP_fft_zripD` (radix-2 split-complex real FFT,
//      hand-tuned NEON on Apple Silicon).
//   2. Otherwise (arbitrary even length): a 2N-point real FFT is built
//      from one N-point complex FFT plus an O(N) untwiddle pass —
//      `ComplexInnerRealFFT` (`ComplexInnerRealFFT.swift`). The inner
//      complex FFT is itself routed here, in priority order:
//      a. `VDSPComplexDFT` (`VDSPComplexDFT.swift`) — `vDSP_DFT_zopD`
//         for sizes `f·2ᵐ`, `f ∈ {1, 3, 5, 15}`, `m ≥ 3`.
//      b. `MixedRadixFFT` (`MixedRadixFFT.swift`) — native mixed-radix
//         for prime factorisations in `{2, 3, 5, 7}`. Its radix-2/4/8
//         stages are NOT redundant with branch (1): they handle the
//         *power-of-two portion* of a mixed factorisation (e.g.
//         `1120 = 2⁵·5·7` factored as `[8, 4, 5, 7]`). Without them
//         MixedRadix could only support odd-only sizes like
//         `105 = 3·5·7`.
//      c. `BluesteinFFT` (`BluesteinFFT.swift`) — universal fallback
//         for anything with a prime factor `> 7` (e.g. our `11→13k`
//         rate pair, halfN = 1034 has primes 11 and 47).
//
// Every backend exposes the same external semantics — forward =
// unscaled DFT, inverse = `length · signal` — so the resampler is
// oblivious to which path runs.
//
// Algorithm references:
//   - https://www.dsprelated.com/showarticle/4.php (Real FFT from complex FFT)
//   - https://en.wikipedia.org/wiki/Fast_Fourier_transform#Real-input_FFTs

#include "FFT/bluestein_fft.h"
#include "FFT/complex_inner_real_fft.h"
#include "FFT/mixed_radix_fft.h"
#include "FFT/vdsp_complex_dft.h"
#include "FFT/vdsp_real_fft.h"

real_fft_t* real_fft_create(size_t length, config_error_t* err) {
  if (length == 0) {
    config_error_set(err, CONFIG_ERR_PARSE, "RealFFT: length must be positive");
    return NULL;
  }
  if (length % 2 != 0) {
    config_error_set(err, CONFIG_ERR_PARSE, "RealFFT: length must be even, got %zu", length);
    return NULL;
  }
  real_fft_t* fft = (real_fft_t*)malloc(sizeof(real_fft_t));
  if (!fft) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate RealFFT");
    return NULL;
  }
  fft->length = length;
  fft->spectrum_length = length / 2 + 1;

  // Branch 1: power-of-2 → vDSP's tuned real FFT, no complex-inner
  // detour. `length >= 8` is the smallest size `vDSP_fft_zripD`
  // supports; smaller pow2 lengths fall through to branch 2.
  vdsp_real_fft_t* vdsp = vdsp_real_fft_create(length);
  if (vdsp) {
    fft->backend = vdsp_real_fft_as_backend(vdsp);
    return fft;
  }

  // Branch 2: even but not power-of-2 (or pow2 < 8). Build the
  // 2N-point real FFT from an N-point complex FFT. Pick the inner
  // complex FFT once, here, in priority order — `ComplexInnerRealFFT`
  // itself just consumes the chosen `inner`.
  size_t half_n = length / 2;
  arbitrary_complex_fft_t* inner = NULL;
  vdsp_complex_dft_t* dft = vdsp_complex_dft_create(half_n);
  if (dft) {
    inner = vdsp_complex_dft_as_arbitrary(dft);
  } else {
    mixed_radix_fft_t* mr = mixed_radix_fft_create(half_n);
    if (mr) {
      inner = mixed_radix_fft_as_arbitrary(mr);
    } else {
      bluestein_fft_t* bs = bluestein_fft_create(half_n, err);
      if (bs) {
        inner = bluestein_fft_as_arbitrary(bs);
      }
    }
  }

  if (!inner) {
    free(fft);
    return NULL;
  }

  complex_inner_real_fft_t* complex_inner =
      complex_inner_real_fft_create(length, inner);
  if (!complex_inner) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate ComplexInnerRealFFT");
    arbitrary_complex_fft_free(inner);
    free(fft);
    return NULL;
  }

  fft->backend = complex_inner_real_fft_as_backend(complex_inner);
  return fft;
}

// Single-precision (float) Accelerate context
struct real_fftf {
  size_t length;
  size_t spectrum_length;
  real_fftf_backend_t* backend;
};

size_t real_fftf_get_length(const real_fftf_t* fft) {
  return fft ? fft->length : 0;
}

size_t real_fftf_get_spectrum_length(const real_fftf_t* fft) {
  return fft ? fft->spectrum_length : 0;
}

real_fftf_t* real_fftf_create(size_t length) {
  if (length == 0 || length % 2 != 0) return NULL;
  real_fftf_t* fft = (real_fftf_t*)malloc(sizeof(real_fftf_t));
  if (!fft) return NULL;
  fft->length = length;
  fft->spectrum_length = length / 2 + 1;

  vdsp_real_fftf_t* vdsp = vdsp_real_fftf_create(length);
  if (vdsp) {
    fft->backend = vdsp_real_fftf_as_backend(vdsp);
    return fft;
  }

  free(fft);
  return NULL;
}

#elif defined(ENABLE_FFTW)

#include <complex.h>
#include <fftw3.h>

struct fftw_real_fft_ctx {
  real_fft_backend_t base;
  size_t length;
  size_t spectrum_length;
  double* in_real;
  fftw_complex* out_complex;
  fftw_plan plan_forward;
  fftw_plan plan_inverse;
};

/**
 * @brief Forward FFT implementation using FFTW.
 *
 * Copies input to FFTW input buffer, executes plan, and copies results to
 * output.
 *
 * @param ctx Pointer to the fftw_real_fft_ctx.
 * @param real_in Input real waveform.
 * @param spec_re Output real part of the spectrum.
 * @param spec_im Output imaginary part of the spectrum.
 */
static void fftw_real_fft_forward(void* ctx, waveform_t real_in,
                                  mutable_waveform_t spec_re,
                                  mutable_waveform_t spec_im) {
  struct fftw_real_fft_ctx* fft = (struct fftw_real_fft_ctx*)ctx;
  memcpy(fft->in_real, real_in, fft->length * sizeof(double));
  fftw_execute(fft->plan_forward);
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    spec_re[i] = __real__(fft->out_complex[i]);
    spec_im[i] = __imag__(fft->out_complex[i]);
  }
}

/**
 * @brief Inverse FFT implementation using FFTW.
 *
 * Copies input spectrum to FFTW complex buffer, executes plan, and copies
 * results to output.
 *
 * @param ctx Pointer to the fftw_real_fft_ctx.
 * @param spec_re Input real part of the spectrum.
 * @param spec_im Input imaginary part of the spectrum.
 * @param real_out Output real waveform.
 */
static void fftw_real_fft_inverse(void* ctx, waveform_t spec_re,
                                  waveform_t spec_im,
                                  mutable_waveform_t real_out) {
  struct fftw_real_fft_ctx* fft = (struct fftw_real_fft_ctx*)ctx;
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    __real__(fft->out_complex[i]) = spec_re[i];
    __imag__(fft->out_complex[i]) = spec_im[i];
  }
  fftw_execute(fft->plan_inverse);
  memcpy(real_out, fft->in_real, fft->length * sizeof(double));
}

/**
 * @brief Free FFTW resources.
 *
 * Destroys plans and frees allocated buffers.
 *
 * @param ctx Pointer to the fftw_real_fft_ctx.
 */
static void fftw_real_fft_free(void* ctx) {
  struct fftw_real_fft_ctx* fft = (struct fftw_real_fft_ctx*)ctx;
  if (!fft) return;
  if (fft->plan_forward) fftw_destroy_plan(fft->plan_forward);
  if (fft->plan_inverse) fftw_destroy_plan(fft->plan_inverse);
  if (fft->in_real) fftw_free(fft->in_real);
  if (fft->out_complex) fftw_free(fft->out_complex);
  free(fft);
}
real_fft_t* real_fft_create(size_t length, config_error_t* err) {
  if (length == 0) {
    config_error_set(err, CONFIG_ERR_PARSE, "RealFFT: length must be positive");
    return NULL;
  }
  if (length % 2 != 0) {
    config_error_set(err, CONFIG_ERR_PARSE, "RealFFT: length must be even, got %zu", length);
    return NULL;
  }
  real_fft_t* fft = (real_fft_t*)calloc(1, sizeof(real_fft_t));
  if (!fft) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate RealFFT");
    return NULL;
  }
  fft->length = length;
  fft->spectrum_length = length / 2 + 1;

  struct fftw_real_fft_ctx* ctx =
      (struct fftw_real_fft_ctx*)calloc(1, sizeof(struct fftw_real_fft_ctx));
  if (!ctx) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate FFTW context");
    free(fft);
    return NULL;
  }
  ctx->length = length;
  ctx->spectrum_length = length / 2 + 1;
  ctx->in_real = (double*)fftw_malloc(length * sizeof(double));
  ctx->out_complex =
      (fftw_complex*)fftw_malloc(ctx->spectrum_length * sizeof(fftw_complex));
  if (!ctx->in_real || !ctx->out_complex) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate FFTW buffers");
    fftw_real_fft_free(ctx);
    free(fft);
    return NULL;
  }
  ctx->plan_forward = fftw_plan_dft_r2c_1d((int)length, ctx->in_real,
                                           ctx->out_complex, FFTW_ESTIMATE);
  ctx->plan_inverse = fftw_plan_dft_c2r_1d((int)length, ctx->out_complex,
                                           ctx->in_real, FFTW_ESTIMATE);
  if (!ctx->plan_forward || !ctx->plan_inverse) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to create FFTW plan");
    fftw_real_fft_free(ctx);
    free(fft);
    return NULL;
  }
  ctx->base.ctx = ctx;
  ctx->base.forward = fftw_real_fft_forward;
  ctx->base.inverse = fftw_real_fft_inverse;
  ctx->base.free = fftw_real_fft_free;

  fft->backend = &ctx->base;
  return fft;
}

// Single-precision (float) FFTW context and implementation
struct real_fftf {
  size_t length;
  size_t spectrum_length;
  real_fftf_backend_t* backend;
};

struct fftwf_real_fft_ctx {
  real_fftf_backend_t base;
  size_t length;
  size_t spectrum_length;
  float* in_real;
  fftwf_complex* out_complex;
  fftwf_plan plan_forward;
  fftwf_plan plan_inverse;
};

static void fftwf_real_fft_forward(void* ctx, const float* real_in,
                                   float* spec_re, float* spec_im) {
  struct fftwf_real_fft_ctx* fft = (struct fftwf_real_fft_ctx*)ctx;
  memcpy(fft->in_real, real_in, fft->length * sizeof(float));
  fftwf_execute(fft->plan_forward);
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    spec_re[i] = __real__(fft->out_complex[i]);
    spec_im[i] = __imag__(fft->out_complex[i]);
  }
}

static void fftwf_real_fft_inverse(void* ctx, const float* spec_re,
                                   const float* spec_im, float* real_out) {
  struct fftwf_real_fft_ctx* fft = (struct fftwf_real_fft_ctx*)ctx;
  for (size_t i = 0; i < fft->spectrum_length; i++) {
    __real__(fft->out_complex[i]) = spec_re[i];
    __imag__(fft->out_complex[i]) = spec_im[i];
  }
  fftwf_execute(fft->plan_inverse);
  memcpy(real_out, fft->in_real, fft->length * sizeof(float));
}

static void fftwf_real_fft_free(void* ctx) {
  struct fftwf_real_fft_ctx* fft = (struct fftwf_real_fft_ctx*)ctx;
  if (!fft) return;
  if (fft->plan_forward) fftwf_destroy_plan(fft->plan_forward);
  if (fft->plan_inverse) fftwf_destroy_plan(fft->plan_inverse);
  if (fft->in_real) fftwf_free(fft->in_real);
  if (fft->out_complex) fftwf_free(fft->out_complex);
  free(fft);
}

size_t real_fftf_get_length(const real_fftf_t* fft) {
  return fft ? fft->length : 0;
}

size_t real_fftf_get_spectrum_length(const real_fftf_t* fft) {
  return fft ? fft->spectrum_length : 0;
}

real_fftf_t* real_fftf_create(size_t length) {
  if (length == 0 || length % 2 != 0) return NULL;
  real_fftf_t* fft = (real_fftf_t*)calloc(1, sizeof(real_fftf_t));
  if (!fft) return NULL;
  fft->length = length;
  fft->spectrum_length = length / 2 + 1;

  struct fftwf_real_fft_ctx* ctx =
      (struct fftwf_real_fft_ctx*)calloc(1, sizeof(struct fftwf_real_fft_ctx));
  if (!ctx) {
    free(fft);
    return NULL;
  }
  ctx->length = length;
  ctx->spectrum_length = length / 2 + 1;
  ctx->in_real = (float*)fftwf_malloc(length * sizeof(float));
  ctx->out_complex = (fftwf_complex*)fftwf_malloc(ctx->spectrum_length *
                                                  sizeof(fftwf_complex));
  if (!ctx->in_real || !ctx->out_complex) {
    fftwf_real_fft_free(ctx);
    free(fft);
    return NULL;
  }
  ctx->plan_forward = fftwf_plan_dft_r2c_1d((int)length, ctx->in_real,
                                            ctx->out_complex, FFTW_ESTIMATE);
  ctx->plan_inverse = fftwf_plan_dft_c2r_1d((int)length, ctx->out_complex,
                                            ctx->in_real, FFTW_ESTIMATE);
  if (!ctx->plan_forward || !ctx->plan_inverse) {
    fftwf_real_fft_free(ctx);
    free(fft);
    return NULL;
  }
  ctx->base.ctx = ctx;
  ctx->base.forward = fftwf_real_fft_forward;
  ctx->base.inverse = fftwf_real_fft_inverse;
  ctx->base.free = fftwf_real_fft_free;

  fft->backend = (real_fftf_backend_t*)&ctx->base;
  return fft;
}

#else
#error "No FFT backend enabled! Enable either ENABLE_ACCELERATE or ENABLE_FFTW."
#endif

// Public API implementation (shared across Apple and non-Apple backends)
void real_fft_forward(real_fft_t* fft, waveform_t real_in,
                      mutable_waveform_t spec_re, mutable_waveform_t spec_im) {
  if (fft && fft->backend && fft->backend->forward) {
    fft->backend->forward(fft->backend->ctx, real_in, spec_re, spec_im);
  }
}

void real_fft_inverse(real_fft_t* fft, waveform_t spec_re, waveform_t spec_im,
                      mutable_waveform_t real_out) {
  if (fft && fft->backend && fft->backend->inverse) {
    fft->backend->inverse(fft->backend->ctx, spec_re, spec_im, real_out);
  }
}

void real_fft_free(real_fft_t* fft) {
  if (fft) {
    if (fft->backend && fft->backend->free) {
      fft->backend->free(fft->backend->ctx);
    }
    free(fft);
  }
}

void real_fftf_forward(real_fftf_t* fft, const float* real_in, float* spec_re,
                       float* spec_im) {
  if (fft && fft->backend && fft->backend->forward) {
    fft->backend->forward(fft->backend->ctx, real_in, spec_re, spec_im);
  }
}

void real_fftf_inverse(real_fftf_t* fft, const float* spec_re,
                       const float* spec_im, float* real_out) {
  if (fft && fft->backend && fft->backend->inverse) {
    fft->backend->inverse(fft->backend->ctx, spec_re, spec_im, real_out);
  }
}

void real_fftf_free(real_fftf_t* fft) {
  if (fft) {
    if (fft->backend && fft->backend->free) {
      fft->backend->free(fft->backend->ctx);
    }
    free(fft);
  }
}
