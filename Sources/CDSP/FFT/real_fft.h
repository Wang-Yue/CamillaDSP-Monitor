#ifndef CLIB_FFT_REALFFT_H
#define CLIB_FFT_REALFFT_H

/**
 * @file real_fft.h
 * @brief Real-input FFT of arbitrary even length.
 *
 * `RealFFT.init` is the **single dispatch point** for the resampler's FFT
 * subsystem — it inspects the requested length once and picks the fastest
 * available backend, so callers (and the per-backend classes) never repeat that
 * decision.
 *
 * Decision tree (top-to-bottom, first match wins):
 *   1. `length` is a power of two `≥ 8`
 *      → `VDSPRealFFT` (`VDSPRealFFT.swift`), wrapping Apple's
 *      `vDSP_fft_zrip` / `vDSP_fft_zripD` (radix-2 split-complex real FFT,
 *      hand-tuned NEON on Apple Silicon).
 *   2. Otherwise (arbitrary even length): a 2N-point real FFT is built
 *      from one N-point complex FFT plus an O(N) untwiddle pass —
 *      `ComplexInnerRealFFT` (`ComplexInnerRealFFT.swift`). The inner
 *      complex FFT is itself routed here, in priority order:
 *      a. `VDSPComplexDFT` (`VDSPComplexDFT.swift`) — `vDSP_DFT_zopD`
 *         for sizes `f·2ᵐ`, `f ∈ {1, 3, 5, 15}`, `m ≥ 3`.
 *      b. `MixedRadixFFT` (`MixedRadixFFT.swift`) — native mixed-radix
 *         for prime factorisations in `{2, 3, 5, 7}`. Its radix-2/4/8
 *         stages are NOT redundant with branch (1): they handle the
 *         *power-of-two portion* of a mixed factorisation (e.g.
 *         `1120 = 2⁵·5·7` factored as `[8, 4, 5, 7]`). Without them
 *         MixedRadix could only support odd-only sizes like
 *         `105 = 3·5·7`.
 *      c. `BluesteinFFT` (`BluesteinFFT.swift`) — universal fallback
 *         for anything with a prime factor `> 7` (e.g. our `11→13k`
 *         rate pair, halfN = 1034 has primes 11 and 47).
 *
 * Every backend exposes the same external semantics — forward =
 * unscaled DFT, inverse = `length · signal` — so the resampler is
 * oblivious to which path runs.
 *
 * Algorithm references:
 *   - https://www.dsprelated.com/showarticle/4.php (Real FFT from complex FFT)
 *   - https://en.wikipedia.org/wiki/Fast_Fourier_transform#Real-input_FFTs
 */

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "Config/config_error.h"

/**
 * @brief Function pointer type for the backend forward FFT execution.
 *
 * @param ctx Pointer to the backend's internal context structure.
 * @param real_in Input buffer of real samples.
 * @param spec_re Output buffer for the real parts of the spectrum.
 * @param spec_im Output buffer for the imaginary parts of the spectrum.
 */
typedef void (*real_fft_backend_forward_fn)(void* ctx, waveform_t real_in,
                                            mutable_waveform_t spec_re,
                                            mutable_waveform_t spec_im);

/**
 * @brief Function pointer type for the backend inverse FFT execution.
 *
 * @param ctx Pointer to the backend's internal context structure.
 * @param spec_re Input buffer for the real parts of the spectrum.
 * @param spec_im Input buffer for the imaginary parts of the spectrum.
 * @param real_out Output buffer for the reconstructed real samples.
 */
typedef void (*real_fft_backend_inverse_fn)(void* ctx, waveform_t spec_re,
                                            waveform_t spec_im,
                                            mutable_waveform_t real_out);

/**
 * @brief Function pointer type for freeing the backend context.
 *
 * @param ctx Pointer to the backend's internal context structure.
 */
typedef void (*real_fft_backend_free_fn)(void* ctx);

/**
 * @struct real_fft_backend
 * @brief Structure defining the interface for a real FFT backend.
 *
 * Module-internal protocol implemented by every real-FFT backend
 * `RealFFT` can dispatch to. Forward = unscaled DFT, inverse
 * = `length · signal` (round-trip with `forward` multiplies by
 * `length`). The protocol-witness call is paid once per `forward` /
 * `inverse` (twice per resampler chunk per channel) and is invisible
 * against the actual FFT cost.
 */
typedef struct {
  void* ctx; /**< Opaque pointer to the backend's internal context. */
  real_fft_backend_forward_fn
      forward; /**< Function to execute the forward FFT. */
  real_fft_backend_inverse_fn
      inverse;                   /**< Function to execute the inverse FFT. */
  real_fft_backend_free_fn free; /**< Function to free the backend context. */
} real_fft_backend_t;

/**
 * @struct real_fft_t
 * @brief Main real FFT structure.
 */
typedef struct real_fft real_fft_t;

/**
 * @brief Get the time-domain length of the real FFT.
 * @param fft Pointer to the real FFT context.
 * @return The length.
 */
size_t real_fft_get_length(const real_fft_t* fft);

/**
 * @brief Get the spectrum length of the real FFT (number of complex bins).
 * @param fft Pointer to the real FFT context.
 * @return The spectrum length.
 */
size_t real_fft_get_spectrum_length(const real_fft_t* fft);

/**
 * @brief Creates a real FFT context for the specified length.
 *
 * This function chooses and instantiates the most appropriate backend
 * based on the requested length.
 *
 * @param length The time-domain length (must be even).
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created real_fft_t context, or NULL on failure.
 */
real_fft_t* real_fft_create(size_t length, config_error_t* err);

/**
 * @brief Computes the forward 2N-point real FFT.
 *
 * Produces the `N + 1` unique complex bins.
 *
 * @param fft The real FFT context.
 * @param real_in Input buffer of real samples (length >= fft->length).
 * @param spec_re Output buffer for the real parts of the spectrum (length >=
 * fft->spectrum_length).
 * @param spec_im Output buffer for the imaginary parts of the spectrum (length
 * >= fft->spectrum_length).
 */
void real_fft_forward(real_fft_t* fft, waveform_t real_in,
                      mutable_waveform_t spec_re, mutable_waveform_t spec_im);

/**
 * @brief Computes the inverse 2N-point real FFT.
 *
 * Reads the `N + 1` unique complex bins from `spec_re`/`spec_im` and writes
 * `length` real samples into `real_out`. Output is scaled by `length`.
 *
 * @param fft The real FFT context.
 * @param spec_re Input buffer for the real parts of the spectrum (length >=
 * fft->spectrum_length).
 * @param spec_im Input buffer for the imaginary parts of the spectrum (length
 * >= fft->spectrum_length).
 * @param real_out Output buffer for the reconstructed real samples (length >=
 * fft->length).
 */
void real_fft_inverse(real_fft_t* fft, waveform_t spec_re, waveform_t spec_im,
                      mutable_waveform_t real_out);

/**
 * @brief Frees the real FFT context and its backend.
 *
 * @param fft The context to destroy.
 */
void real_fft_free(real_fft_t* fft);

// Single-precision (float) Real FFT declarations
typedef struct real_fftf real_fftf_t;

typedef void (*real_fftf_backend_forward_fn)(void* ctx, const float* real_in,
                                             float* spec_re, float* spec_im);
typedef void (*real_fftf_backend_inverse_fn)(void* ctx, const float* spec_re,
                                             const float* spec_im,
                                             float* real_out);
typedef void (*real_fftf_backend_free_fn)(void* ctx);

typedef struct {
  void* ctx;
  real_fftf_backend_forward_fn forward;
  real_fftf_backend_inverse_fn inverse;
  real_fftf_backend_free_fn free;
} real_fftf_backend_t;

size_t real_fftf_get_length(const real_fftf_t* fft);
size_t real_fftf_get_spectrum_length(const real_fftf_t* fft);
real_fftf_t* real_fftf_create(size_t length);
void real_fftf_forward(real_fftf_t* fft, const float* real_in, float* spec_re,
                       float* spec_im);
void real_fftf_inverse(real_fftf_t* fft, const float* spec_re,
                       const float* spec_im, float* real_out);
void real_fftf_free(real_fftf_t* fft);

#endif  // CLIB_FFT_REALFFT_H
