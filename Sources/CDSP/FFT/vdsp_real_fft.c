// vDSP `fft_zrip` backend for power-of-two real-FFT lengths.
//
// Selected by `RealFFT.init` when `length` is a power of two
// `≥ 8`. vDSP's hand-tuned NEON/SSE radix-2 split-complex real FFT is
// the fastest path on Apple Silicon — for our resampler matrix it
// roughly doubles the throughput of the "complex-FFT-via-half-N" path
// for sizes like 1024/2048/4096.

#include "FFT/vdsp_real_fft.h"
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

struct vdsp_real_fft {
    real_fft_backend_t base;
    size_t half_n;
#ifdef __APPLE__
    vDSP_Length log2n;
    FFTSetupD setup;
#endif
    double* scratch_re;
    double* scratch_im;
};

static void vdsp_real_fft_forward_wrapper(void* ctx, waveform_t real_in, mutable_waveform_t spec_re, mutable_waveform_t spec_im) {
    vdsp_real_fft_forward((vdsp_real_fft_t*)ctx, real_in, spec_re, spec_im);
}

static void vdsp_real_fft_inverse_wrapper(void* ctx, waveform_t spec_re, waveform_t spec_im, mutable_waveform_t real_out) {
    vdsp_real_fft_inverse((vdsp_real_fft_t*)ctx, spec_re, spec_im, real_out);
}

static void vdsp_real_fft_free_wrapper(void* ctx) {
    vdsp_real_fft_free((vdsp_real_fft_t*)ctx);
}

vdsp_real_fft_t* vdsp_real_fft_create(size_t length) {
    if (length < 8 || (length & (length - 1)) != 0) return NULL;

#ifdef __APPLE__
    vDSP_Length log2n = 0;
    size_t temp = length;
    while (temp > 1) {
        log2n++;
        temp >>= 1;
    }
    FFTSetupD setup = vDSP_create_fftsetupD(log2n, kFFTRadix2);
    if (!setup) return NULL;

    vdsp_real_fft_t* fft = (vdsp_real_fft_t*)malloc(sizeof(vdsp_real_fft_t));
    if (!fft) {
        vDSP_destroy_fftsetupD(setup);
        return NULL;
    }
    size_t half_n = length / 2;
    fft->base.ctx = fft;
    fft->base.forward = vdsp_real_fft_forward_wrapper;
    fft->base.inverse = vdsp_real_fft_inverse_wrapper;
    fft->base.free = vdsp_real_fft_free_wrapper;
    fft->half_n = half_n;
    fft->log2n = log2n;
    fft->setup = setup;
    fft->scratch_re = (double*)malloc(half_n * sizeof(double));
    fft->scratch_im = (double*)malloc(half_n * sizeof(double));
    if (!fft->scratch_re || !fft->scratch_im) {
        vdsp_real_fft_free(fft);
        return NULL;
    }
    return fft;
#else
    (void)length;
    return NULL;
#endif
}

void vdsp_real_fft_forward(vdsp_real_fft_t* fft, waveform_t real_in, mutable_waveform_t spec_re, mutable_waveform_t spec_im) {
    if (!fft) return;
#ifdef __APPLE__
    size_t n = fft->half_n;
    // Deinterleave 2N real samples into N split-complex pairs:
    // scratch.real[k] = realIn[2k], scratch.imag[k] = realIn[2k+1].
    DSPDoubleSplitComplex split = { fft->scratch_re, fft->scratch_im };
    vDSP_ctozD((const DSPDoubleComplex*)real_in, 2, &split, 1, (vDSP_Length)n);
    // In-place real-to-complex forward FFT. vDSP scales by 2.
    vDSP_fft_zripD(fft->setup, &split, 1, fft->log2n, FFT_FORWARD);

    // Repack vDSP's packed spectrum into our flat (N+1)-bin layout, folding
    // the `0.5` un-scale into the copy. After:
    //   specRe[0]   = vDSP_DC / 2 = unscaled DC
    //   specIm[0]   = 0
    //   specRe[k]   = vDSP_Re[k] / 2   for k = 1..N-1
    //   specIm[k]   = vDSP_Im[k] / 2   for k = 1..N-1
    //   specRe[N]   = vDSP_Im[0] / 2   (Nyquist was packed in imagp[0])
    //   specIm[N]   = 0
    double half = 0.5;
    vDSP_vsmulD(fft->scratch_re, 1, &half, spec_re, 1, (vDSP_Length)n);
    if (n > 1) {
        vDSP_vsmulD(fft->scratch_im + 1, 1, &half, spec_im + 1, 1, (vDSP_Length)(n - 1));
    }
    spec_im[0] = 0.0;
    spec_re[n] = fft->scratch_im[0] * 0.5;
    spec_im[n] = 0.0;
#else
    (void)real_in; (void)spec_re; (void)spec_im;
#endif
}

void vdsp_real_fft_inverse(vdsp_real_fft_t* fft, waveform_t spec_re, waveform_t spec_im, mutable_waveform_t real_out) {
    if (!fft) return;
#ifdef __APPLE__
    size_t n = fft->half_n;
    // Repack our flat (N+1)-bin layout back into vDSP's packed format
    // (DC in realp[0], Nyquist in imagp[0], bins 1..N-1 in realp[k]/imagp[k]).
    fft->scratch_re[0] = spec_re[0];
    fft->scratch_im[0] = spec_re[n];
    if (n > 1) {
        memcpy(fft->scratch_re + 1, spec_re + 1, (n - 1) * sizeof(double));
        memcpy(fft->scratch_im + 1, spec_im + 1, (n - 1) * sizeof(double));
    }

    DSPDoubleSplitComplex split = { fft->scratch_re, fft->scratch_im };
    vDSP_fft_zripD(fft->setup, &split, 1, fft->log2n, FFT_INVERSE);

    // Asymmetric vDSP scaling: forward applies a `2×` factor, inverse
    // does not. Feeding unscaled bins (we already halved the forward
    // output) directly produces the unnormalised IDFT result —
    // `length · signal` — which is exactly the RealFFT
    // convention. No extra scaling needed here.
    //
    // Re-interleave split-complex back to 2N reals: realOut[2k] = split.real[k],
    // realOut[2k+1] = split.imag[k].
    vDSP_ztocD(&split, 1, (DSPDoubleComplex*)real_out, 2, (vDSP_Length)n);
#else
    (void)spec_re; (void)spec_im; (void)real_out;
#endif
}

void vdsp_real_fft_free(vdsp_real_fft_t* fft) {
    if (!fft) return;
#ifdef __APPLE__
    if (fft->setup) vDSP_destroy_fftsetupD(fft->setup);
#endif
    if (fft->scratch_re) free(fft->scratch_re);
    if (fft->scratch_im) free(fft->scratch_im);
    free(fft);
}
