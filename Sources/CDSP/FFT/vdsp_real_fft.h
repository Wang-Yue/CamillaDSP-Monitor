#ifndef CLIB_FFT_VDSPREALFFT_H
#define CLIB_FFT_VDSPREALFFT_H

// vDSP `fft_zrip` backend for power-of-two real-FFT lengths.
//
// Selected by `RealFFT.init` when `length` is a power of two
// `≥ 8`. vDSP's hand-tuned NEON/SSE radix-2 split-complex real FFT is
// the fastest path on Apple Silicon — for our resampler matrix it
// roughly doubles the throughput of the "complex-FFT-via-half-N" path
// for sizes like 1024/2048/4096.

#include <stdbool.h>
#include <stddef.h>

#include "Audio/double_helpers.h"
#include "real_fft.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Wraps Apple's `vDSP_fft_zripD` (radix-2 split-complex real FFT). vDSP's
/// internal scaling is asymmetric — forward applies a `2×` factor, inverse
/// does not — so we fold a `0.5` factor into the spectrum unpack on the
/// forward path. The externally observed semantics then match
/// `ComplexInnerRealFFT` exactly: forward = unscaled DFT, inverse =
/// `length · signal`.
///
/// vDSP's spectrum packing: DC is in `realp[0]`, Nyquist in `imagp[0]`,
/// bins `1..N-1` in `realp[k] + i·imagp[k]`. Our public API exposes the
/// `N+1` unique bins as flat `specRe`/`specIm` arrays with DC at index 0,
/// Nyquist at index N — this backend repacks accordingly.
typedef struct vdsp_real_fft vdsp_real_fft_t;

/// Returns `nil` when `length` is not a power of two `≥ 8`, or when
/// `vDSP_create_fftsetupD` fails — caller falls back to the
/// complex-inner backend.
vdsp_real_fft_t* vdsp_real_fft_create(size_t length);
void vdsp_real_fft_forward(vdsp_real_fft_t* fft, waveform_t real_in,
                           mutable_waveform_t spec_re,
                           mutable_waveform_t spec_im);
void vdsp_real_fft_inverse(vdsp_real_fft_t* fft, waveform_t spec_re,
                           waveform_t spec_im, mutable_waveform_t real_out);
void vdsp_real_fft_free(vdsp_real_fft_t* fft);

static inline real_fft_backend_t* vdsp_real_fft_as_backend(
    vdsp_real_fft_t* fft) {
  return (real_fft_backend_t*)fft;
}

#ifdef __cplusplus
}
#endif

#endif  // CLIB_FFT_VDSPREALFFT_H
