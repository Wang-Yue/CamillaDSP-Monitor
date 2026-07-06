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

#include "FFT/real_fft.h"
#include "FFT/vdsp_real_fft.h"
#include "FFT/complex_inner_real_fft.h"
#include "FFT/vdsp_complex_dft.h"
#include "FFT/mixed_radix_fft.h"
#include "FFT/bluestein_fft.h"
#include <stdlib.h>

real_fft_t* real_fft_create(size_t length) {
    if (length == 0 || length % 2 != 0) return NULL;
    real_fft_t* fft = (real_fft_t*)malloc(sizeof(real_fft_t));
    if (!fft) return NULL;
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
            bluestein_fft_t* bs = bluestein_fft_create(half_n);
            if (bs) {
                inner = bluestein_fft_as_arbitrary(bs);
            }
        }
    }

    if (!inner) {
        free(fft);
        return NULL;
    }

    complex_inner_real_fft_t* complex_inner = complex_inner_real_fft_create(length, inner);
    if (!complex_inner) {
        arbitrary_complex_fft_free(inner);
        free(fft);
        return NULL;
    }

    fft->backend = complex_inner_real_fft_as_backend(complex_inner);
    return fft;
}

void real_fft_forward(real_fft_t* fft, waveform_t real_in, mutable_waveform_t spec_re, mutable_waveform_t spec_im) {
    if (fft && fft->backend && fft->backend->forward) {
        fft->backend->forward(fft->backend->ctx, real_in, spec_re, spec_im);
    }
}

void real_fft_inverse(real_fft_t* fft, waveform_t spec_re, waveform_t spec_im, mutable_waveform_t real_out) {
    if (fft && fft->backend && fft->backend->inverse) {
        fft->backend->inverse(fft->backend->ctx, spec_re, spec_im, real_out);
    }
}

void real_fft_free(real_fft_t* fft) {
    if (!fft) return;
    if (fft->backend && fft->backend->free) {
        fft->backend->free(fft->backend->ctx);
    }
    free(fft);
}
