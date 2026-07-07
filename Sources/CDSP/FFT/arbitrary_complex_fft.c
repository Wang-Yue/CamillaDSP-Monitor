// Shared interface for any complex-input/output DFT engine. The
// `ComplexInnerRealFFT` real-FFT backend takes one of these as its
// inner transform; `RealFFT.init` does the priority-based
// selection between the available conformers.

#include "FFT/arbitrary_complex_fft.h"

#include <stdlib.h>

void arbitrary_complex_fft_execute(arbitrary_complex_fft_t* fft,
                                   waveform_t real_in, waveform_t imag_in,
                                   mutable_waveform_t real_out,
                                   mutable_waveform_t imag_out, bool inverse) {
  if (fft && fft->execute) {
    fft->execute(fft->ctx, real_in, imag_in, real_out, imag_out, inverse);
  }
}

void arbitrary_complex_fft_free(arbitrary_complex_fft_t* fft) {
  if (fft) {
    if (fft->free) {
      fft->free(fft->ctx);
    }
  }
}
