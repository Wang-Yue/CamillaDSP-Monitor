#ifndef CLIB_FFT_REALFFT_H
#define CLIB_FFT_REALFFT_H

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
//      `vDSP_fft_zripD` (radix-2 split-complex real FFT, hand-tuned
//      NEON on Apple Silicon).
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

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*real_fft_backend_forward_fn)(void* ctx, waveform_t real_in,
                                            mutable_waveform_t spec_re,
                                            mutable_waveform_t spec_im);
typedef void (*real_fft_backend_inverse_fn)(void* ctx, waveform_t spec_re,
                                            waveform_t spec_im,
                                            mutable_waveform_t real_out);
typedef void (*real_fft_backend_free_fn)(void* ctx);

/// Module-internal protocol implemented by every real-FFT backend
/// `RealFFT` can dispatch to. Forward = unscaled DFT, inverse
/// = `length · signal` (round-trip with `forward` multiplies by
/// `length`). The protocol-witness call is paid once per `forward` /
/// `inverse` (twice per resampler chunk per channel) and is invisible
/// against the actual FFT cost.
typedef struct {
  void* ctx;
  real_fft_backend_forward_fn forward;
  real_fft_backend_inverse_fn inverse;
  real_fft_backend_free_fn free;
} real_fft_backend_t;

/// Real-input/output FFT of length `length = 2N` (even). Forward
/// produces the `N + 1` unique complex bins; inverse consumes them.
/// Caller is responsible for any `1/length` normalisation.
///
/// `init(length:)` is the project's single FFT-backend selector — see
/// the file-level header for the routing decision tree. Callers never
/// see (or pick) a backend; they just get a correctly-sized real FFT.
typedef struct {
  /// Time-domain length (must be even).
  size_t length;
  /// Number of unique complex bins in the spectrum (= length/2 + 1).
  size_t spectrum_length;
  real_fft_backend_t* backend;
} real_fft_t;

real_fft_t* real_fft_create(size_t length);
/// Forward 2N-point real FFT. Produces the `N + 1` unique complex bins.
/// `realIn` length must be ≥ `length`; `specRe`/`specIm` length must be
/// ≥ `spectrumLength`.
void real_fft_forward(real_fft_t* fft, waveform_t real_in,
                      mutable_waveform_t spec_re, mutable_waveform_t spec_im);
/// Inverse 2N-point real FFT. Reads the `N + 1` unique complex bins from
/// `specRe`/`specIm` and writes `length` real samples into `realOut`.
/// Output is scaled by `length` (round-trip with `forward` multiplies by
/// `length`).
void real_fft_inverse(real_fft_t* fft, waveform_t spec_re, waveform_t spec_im,
                      mutable_waveform_t real_out);
void real_fft_free(real_fft_t* fft);

#ifdef __cplusplus
}
#endif

#endif  // CLIB_FFT_REALFFT_H
