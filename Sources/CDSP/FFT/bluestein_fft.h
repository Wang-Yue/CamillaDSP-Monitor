#ifndef CLIB_FFT_BLUESTEINFFT_H
#define CLIB_FFT_BLUESTEINFFT_H

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

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "arbitrary_complex_fft.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Computes the unnormalised forward DFT
///   `X[k] = Σₙ x[n] · exp(-2πi · n · k / N)`
/// or the unnormalised inverse DFT
///   `x[n] = Σₖ X[k] · exp(+2πi · n · k / N)`
/// for arbitrary `N > 0`. Inverse callers that want the true inverse must
/// divide by `N` themselves — both directions are returned scale-free,
/// and the caller (e.g. the synchronous resampler) folds the 1/N
/// normalisation into its filter prefactor.
typedef struct bluestein_fft bluestein_fft_t;

bluestein_fft_t* bluestein_fft_create(size_t n);
/// Run the N-point DFT. `inverse=false` is the forward transform;
/// `inverse=true` is the unnormalised inverse — caller divides by `n` if
/// the textbook 1/N normalisation is wanted.
///
/// Implementation: `IDFT(x) = conj(DFT(conj(x)))`, which lets the inverse
/// path reuse the forward `α` and `bRealF/bImagF` tables — pre-multiply
/// with `Conjugate=-1` (vDSP applies conj to B), post-multiply regular,
/// then negate the imag of the output.
///
/// vDSP convention for `vDSP_zvmulD`'s `Conjugate` arg: `+1` = `C = A · B`,
/// `-1` = `C = A · conj(B)`.
void bluestein_fft_execute(bluestein_fft_t* fft, waveform_t real_in,
                           waveform_t imag_in, mutable_waveform_t real_out,
                           mutable_waveform_t imag_out, bool inverse);
void bluestein_fft_free(bluestein_fft_t* fft);

static inline arbitrary_complex_fft_t* bluestein_fft_as_arbitrary(
    bluestein_fft_t* fft) {
  return (arbitrary_complex_fft_t*)fft;
}

#ifdef __cplusplus
}
#endif

#endif  // CLIB_FFT_BLUESTEINFFT_H
