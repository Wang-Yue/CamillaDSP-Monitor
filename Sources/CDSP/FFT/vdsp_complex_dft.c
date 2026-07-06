// vDSP `DFT_zopD` backend for complex DFTs at sizes `f·2ᵐ`,
// `f ∈ {1, 3, 5, 15}`, `m ≥ 3`. Used by `ComplexInnerRealFFT` as its
// inner transform when the size qualifies — Apple's tuned mixed-radix
// is typically faster than `MixedRadixFFT` in this regime.

#include "FFT/vdsp_complex_dft.h"

#include <stdlib.h>

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

struct vdsp_complex_dft {
  arbitrary_complex_fft_t base;
#ifdef __APPLE__
  vDSP_DFT_SetupD setup_forward;
  vDSP_DFT_SetupD setup_inverse;
#endif
};

#ifdef __APPLE__
static void vdsp_complex_dft_execute_wrapper(void* ctx, waveform_t real_in,
                                             waveform_t imag_in,
                                             mutable_waveform_t real_out,
                                             mutable_waveform_t imag_out,
                                             bool inverse) {
  vdsp_complex_dft_execute((vdsp_complex_dft_t*)ctx, real_in, imag_in, real_out,
                           imag_out, inverse);
}

static void vdsp_complex_dft_free_wrapper(void* ctx) {
  vdsp_complex_dft_free((vdsp_complex_dft_t*)ctx);
}
#endif

vdsp_complex_dft_t* vdsp_complex_dft_create(size_t n) {
#ifdef __APPLE__
  vDSP_DFT_SetupD fwd =
      vDSP_DFT_zop_CreateSetupD(NULL, (vDSP_Length)n, vDSP_DFT_FORWARD);
  if (!fwd) return NULL;
  vDSP_DFT_SetupD inv =
      vDSP_DFT_zop_CreateSetupD(fwd, (vDSP_Length)n, vDSP_DFT_INVERSE);
  if (!inv) {
    vDSP_DFT_DestroySetupD(fwd);
    return NULL;
  }
  vdsp_complex_dft_t* dft =
      (vdsp_complex_dft_t*)malloc(sizeof(vdsp_complex_dft_t));
  if (!dft) {
    vDSP_DFT_DestroySetupD(fwd);
    vDSP_DFT_DestroySetupD(inv);
    return NULL;
  }
  dft->base.ctx = dft;
  dft->base.execute = vdsp_complex_dft_execute_wrapper;
  dft->base.free = vdsp_complex_dft_free_wrapper;
  dft->setup_forward = fwd;
  dft->setup_inverse = inv;
  return dft;
#else
  (void)n;
  return NULL;
#endif
}

void vdsp_complex_dft_execute(vdsp_complex_dft_t* dft, waveform_t real_in,
                              waveform_t imag_in, mutable_waveform_t real_out,
                              mutable_waveform_t imag_out, bool inverse) {
  if (!dft) return;
#ifdef __APPLE__
  vDSP_DFT_SetupD setup = inverse ? dft->setup_inverse : dft->setup_forward;
  vDSP_DFT_ExecuteD(setup, real_in, imag_in, real_out, imag_out);
#else
  (void)real_in;
  (void)imag_in;
  (void)real_out;
  (void)imag_out;
  (void)inverse;
#endif
}

void vdsp_complex_dft_free(vdsp_complex_dft_t* dft) {
  if (!dft) return;
#ifdef __APPLE__
  // Documented as safe in either order; destroying the inverse first
  // mirrors creation order in reverse.
  if (dft->setup_inverse) vDSP_DFT_DestroySetupD(dft->setup_inverse);
  if (dft->setup_forward) vDSP_DFT_DestroySetupD(dft->setup_forward);
#endif
  free(dft);
}
