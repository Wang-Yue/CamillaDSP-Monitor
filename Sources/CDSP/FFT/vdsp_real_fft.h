#ifndef CLIB_FFT_VDSPREALFFT_H
#define CLIB_FFT_VDSPREALFFT_H

/**
 * @file vdsp_real_fft.h
 * @brief vDSP `fft_zrip` backend for power-of-two real-FFT lengths.
 *
 * Selected by `RealFFT.init` when `length` is a power of two `≥ 8`.
 * vDSP's hand-tuned NEON/SSE radix-2 split-complex real FFT is the fastest path
 * on Apple Silicon — for our resampler matrix it roughly doubles the throughput
 * of the "complex-FFT-via-half-N" path for sizes like 1024/2048/4096.
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "real_fft.h"

#if defined(ENABLE_ACCELERATE)
/**
 * @brief Opaque structure wrapping Apple's `vDSP_fft_zripD` state.
 *
 * Wraps Apple's `vDSP_fft_zripD` (radix-2 split-complex real FFT). vDSP's
 * internal scaling is asymmetric — forward applies a `2×` factor, inverse
 * does not — so we fold a `0.5` factor into the spectrum unpack on the
 * forward path. The externally observed semantics then match
 * `ComplexInnerRealFFT` exactly: forward = unscaled DFT, inverse =
 * `length · signal`.
 *
 * vDSP's spectrum packing: DC is in `realp[0]`, Nyquist in `imagp[0]`,
 * bins `1..N-1` in `realp[k] + i·imagp[k]`. Our public API exposes the
 * `N+1` unique bins as flat `specRe`/`specIm` arrays with DC at index 0,
 * Nyquist at index N — this backend repacks accordingly.
 */
typedef struct vdsp_real_fft vdsp_real_fft_t;

/**
 * @brief Creates a vDSP real FFT context.
 *
 * Returns `NULL` when `length` is not a power of two `≥ 8`,
 * or when `vDSP_create_fftsetupD` fails — caller falls back to the
 * complex-inner backend.
 *
 * @param length The length of the FFT. Must be a power of two `≥ 8`.
 * @return A pointer to the created context, or `NULL` on failure.
 */
vdsp_real_fft_t* vdsp_real_fft_create(size_t length);

/**
 * @brief Executes a forward real FFT.
 *
 * @param fft The FFT context.
 * @param real_in Input real signal array of size `length`.
 * @param spec_re Output array for the real parts of the spectrum (size
 * `length/2 + 1`).
 * @param spec_im Output array for the imaginary parts of the spectrum (size
 * `length/2 + 1`).
 */
void vdsp_real_fft_forward(vdsp_real_fft_t* fft, waveform_t real_in,
                           mutable_waveform_t spec_re,
                           mutable_waveform_t spec_im);

/**
 * @brief Executes an inverse real FFT.
 *
 * @param fft The FFT context.
 * @param spec_re Input array for the real parts of the spectrum (size `length/2
 * + 1`).
 * @param spec_im Input array for the imaginary parts of the spectrum (size
 * `length/2 + 1`).
 * @param real_out Output real signal array of size `length`.
 */
void vdsp_real_fft_inverse(vdsp_real_fft_t* fft, waveform_t spec_re,
                           waveform_t spec_im, mutable_waveform_t real_out);

/**
 * @brief Frees the vDSP real FFT context.
 *
 * @param fft The context to free.
 */
void vdsp_real_fft_free(vdsp_real_fft_t* fft);

/**
 * @brief Casts the vDSP real FFT context to a generic real FFT backend
 * interface.
 *
 * @param fft The context to cast.
 * @return A pointer to the generic real FFT backend interface.
 */
static inline real_fft_backend_t* vdsp_real_fft_as_backend(
    vdsp_real_fft_t* fft) {
  return (real_fft_backend_t*)fft;
}

// Single-precision (float) vDSP FFT declarations
typedef struct vdsp_real_fftf vdsp_real_fftf_t;

vdsp_real_fftf_t* vdsp_real_fftf_create(size_t length);
void vdsp_real_fftf_forward(vdsp_real_fftf_t* fft, const float* real_in,
                            float* spec_re, float* spec_im);
void vdsp_real_fftf_inverse(vdsp_real_fftf_t* fft, const float* spec_re,
                            const float* spec_im, float* real_out);
void vdsp_real_fftf_free(vdsp_real_fftf_t* fft);

static inline real_fftf_backend_t* vdsp_real_fftf_as_backend(
    vdsp_real_fftf_t* fft) {
  return (real_fftf_backend_t*)fft;
}

#endif  // ENABLE_ACCELERATE

#endif  // CLIB_FFT_VDSPREALFFT_H
