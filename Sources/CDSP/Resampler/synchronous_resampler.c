// FFT-based fixed-ratio sample-rate converter.
//
// Independently derived from textbook descriptions of FFT-based rate
// conversion via overlap-add convolution and spectral resampling.
//
// References
// ----------
//   * R. E. Crochiere and L. R. Rabiner (1983), "Multirate Digital
//     Signal Processing", Prentice-Hall — §3 covers the L/M
//     decimator-interpolator structure and its block-rate FFT
//     realisation.
//   * A. V. Oppenheim and R. W. Schafer, "Discrete-Time Signal
//     Processing", Prentice-Hall — §4 (sample-rate alteration), §8.7
//     ("Overlap-Save and Overlap-Add Methods" for FFT-based linear
//     convolution), §8.8 (FFT-based fast convolution).
//   * J. O. Smith, "Digital Audio Resampling Home Page", CCRMA —
//     https://ccrma.stanford.edu/~jos/resample/ — covers FFT-based
//     bandlimited interpolation and windowed-sinc filter design.
//   * F. J. Harris (1978), "On the Use of Windows for Harmonic
//     Analysis with the Discrete Fourier Transform", Proc. IEEE
//     vol. 66 no. 1 — Blackman-Harris window (used here via
//     `SincWindowFunction.swift`).
//
// Algorithm
// ---------
// Given input rate `Fᵢ`, output rate `Fₒ`, and `g = gcd(Fᵢ, Fₒ)`,
// define
//
//     L = Fᵢ / g     (input block size in samples per rational period)
//     M = Fₒ / g     (output block size in samples per rational period)
//
// Any integer multiple `N = K·L` input samples corresponds to
// exactly `K·M` output samples — the resampler is fixed-ratio. We
// round the user-supplied `chunkSize` up to the smallest valid
// `K·L`, which fixes the per-call input/output block lengths.
//
// At init, build a windowed-sinc lowpass filter `h[n]` of length `N`
// with cutoff at `min(1, Fₒ/Fᵢ) · π` rad/sample (Crochiere & Rabiner
// §3.1, Smith CCRMA §"Windowed-Sinc Filter Design"), zero-pad to
// length `2N`, and pre-FFT it once into `H`.
//
// Per chunk per channel:
//
//   1. Forward 2N-point real FFT of the zero-padded input. The
//      zero-pad to length 2N converts the otherwise cyclic FFT
//      convolution into a linear convolution — the standard
//      overlap-add structure in Oppenheim & Schafer §8.7.
//
//   2. Multiply pointwise by `H` to apply the anti-aliasing filter
//      in the frequency domain. Cost: O(N) per chunk versus O(N²)
//      for a direct time-domain convolution.
//
//   3. Build the output spectrum of length `2P` where `P = K·M`:
//        — bins 0…min(N, P) get a copy of the filtered input
//          spectrum;
//        — bins above are set to zero.
//      For upsampling (M > L), the zero-pad above input Nyquist is
//      what extends the bandwidth. For downsampling (M < L),
//      truncating to the first `P + 1` unique bins is the
//      band-limiting step. This is the "spectral resampling" of
//      Smith's CCRMA notes.
//
//   4. Inverse 2P-point real FFT recovers a length-2P time-domain
//      block.
//
//   5. Overlap-add: emit `result[0..P) + carry`, save
//      `result[P..2P)` as `carry` for the next chunk
//      (Oppenheim & Schafer §8.7).
//
// The arbitrary-length real FFTs are handled by `RealFFT`,
// which lets the block lengths be sized exactly to `L` and `M`
// rather than padded to a power of two.
//
// Allocation discipline
// ---------------------
// Every per-channel and per-call buffer is allocated once at
// `init`. `process(input:into:)` does no heap allocation and writes
// directly into the caller's pre-allocated `output` chunk.

#include "synchronous_resampler.h"

struct synchronous_resampler {
  /// Number of channels processed per call.
  size_t channels;
  /// Input frames the resampler expects on every `process` call —
  /// `K·L` for some integer `K ≥ 1`, where `L = Fᵢ / gcd(Fᵢ, Fₒ)`.
  size_t chunk_size;
  /// Output frames produced per `process` call — `K·M`, where
  /// `M = Fₒ / gcd(Fᵢ, Fₒ)`.
  size_t output_chunk_size;
  double ratio;
  /// Length of the working FFT block on the input side (`= chunkSize`).
  size_t input_block_len;
  /// Length of the working FFT block on the output side (`= outputChunkSize`).
  size_t output_block_len;
  /// Number of unique-bin frequencies common to the input and output
  /// spectra: `min(inputBlockLen, outputBlockLen) + 1`. Bins above
  /// this in the output spectrum are zeroed (band-limiting for
  /// downsampling, spectral zero-pad for upsampling).
  size_t shared_bins;
  // Anti-aliasing filter, pre-FFT'd at init. `inputBlockLen + 1`
  // unique bins. Stored as raw pointer to bypass overhead.
  double* filter_spec_re;
  double* filter_spec_im;
  // Real-input FFT engines. The forward engine handles the zero-padded
  // input block (length `2 · inputBlockLen`); the inverse engine
  // reconstructs the output block (length `2 · outputBlockLen`).
  real_fft_t* input_fft;
  real_fft_t* output_fft;
  // Per-channel time-domain overlap-add carry. Each entry holds the
  // tail of the previous chunk's IFFT result, length `outputBlockLen`.
  double** carries;
  // Hot-path scratch buffers reused across channels. Unified to minimize
  // cache footprint and avoid intermediate allocations.
  //   `workingTime`: holds the 2N zero-padded input block for forward FFT,
  //                  and the 2P overlap-add output block from inverse FFT.
  //   `workingSpecRe`/`Im`: holds the shared low-frequency bins during
  //   filtering.
  double* working_time;
  double* working_spec_re;
  double* working_spec_im;
};

#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_ACCELERATE
#include <Accelerate/Accelerate.h>
#endif

/**
 * @brief Computes the greatest common divisor (GCD) of two size_t integers
 * using Euclid's algorithm.
 *
 * This helper is used to find the irreducible rational fraction L/M of the
 * sample rate conversion ratio, which determines the input and output chunk
 * size scaling.
 *
 * @param a First value.
 * @param b Second value.
 * @return Greatest common divisor of a and b.
 */
static inline size_t gcd_size(size_t a, size_t b) {
  size_t x = a, y = b;
  while (y != 0) {
    size_t t = y;
    y = x % y;
    x = t;
  }
  return x;
}

synchronous_resampler_t* synchronous_resampler_create(
    size_t channels, size_t input_rate, size_t output_rate,
    size_t requested_chunk_size, config_error_t* err) {
  if (channels == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "SynchronousResampler: channels must be positive");
    return NULL;
  }
  if (requested_chunk_size == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "SynchronousResampler: chunk_size must be positive");
    return NULL;
  }
  if (input_rate == 0 || output_rate == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "SynchronousResampler: rates must be positive");
    return NULL;
  }

  synchronous_resampler_t* resampler =
      (synchronous_resampler_t*)calloc(1, sizeof(synchronous_resampler_t));
  if (!resampler) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate SynchronousResampler");
    return NULL;
  }

  resampler->channels = channels;
  resampler->ratio = (double)output_rate / (double)input_rate;

  // Block-size selection by rational decomposition.
  //   g = gcd(Fᵢ, Fₒ);   L = Fᵢ/g;   M = Fₒ/g
  //   K·L input samples ↔ K·M output samples (exactly, for any K ≥ 1)
  // Round the requested chunkSize up to the smallest valid K·L.
  size_t g = gcd_size(input_rate, output_rate);
  size_t l = input_rate / g;
  size_t m = output_rate / g;
  size_t k = (size_t)ceil((double)requested_chunk_size / (double)l);
  if (k < 1) k = 1;
  size_t input_block = k * l;
  size_t output_block = k * m;

  if (input_block > SIZE_MAX / 2 || output_block > SIZE_MAX / 2) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "SynchronousResampler: block size overflows SIZE_MAX / 2");
    synchronous_resampler_free(resampler);
    return NULL;
  }

  resampler->input_block_len = input_block;
  resampler->output_block_len = output_block;
  resampler->chunk_size = input_block;
  resampler->output_chunk_size = output_block;
  resampler->shared_bins =
      (input_block < output_block ? input_block : output_block) + 1;

  // Build the anti-aliasing kernel. The kernel is applied at input
  // rate, so all frequencies are normalised to input Nyquist.
  double cutoff;
  if (input_rate > output_rate) {
    double base_cut =
        calculate_cutoff(output_block, WINDOW_FUNCTION_BLACKMAN_HARRIS2);
    cutoff = base_cut * (double)output_block / (double)input_block;
  } else {
    cutoff = calculate_cutoff(input_block, WINDOW_FUNCTION_BLACKMAN_HARRIS2);
  }
  double* kernel =
      make_sinc_table(input_block, 1, WINDOW_FUNCTION_BLACKMAN_HARRIS2, cutoff);
  if (!kernel) {
    config_error_set(err, CONFIG_ERR_PARSE, "SynchronousResampler: Failed to build anti-aliasing kernel");
    synchronous_resampler_free(resampler);
    return NULL;
  }

  // Zero-pad the unity-DC-gain kernel into a length-2N buffer for
  // overlap-add convolution (Oppenheim & Schafer §8.7). Pre-scaling
  // by 1/(2·N) folds the unnormalised forward + inverse FFT scale
  // factors into the filter so the resampler delivers unity gain to
  // its callers.
  size_t two_n = 2 * input_block;
  double* filter_time = (double*)calloc(two_n, sizeof(double));
  if (!filter_time) {
    config_error_set(err, CONFIG_ERR_PARSE, "SynchronousResampler: Failed to allocate filter time buffer");
    free(kernel);
    synchronous_resampler_free(resampler);
    return NULL;
  }
  double scale = 1.0 / (double)two_n;
  for (size_t i = 0; i < input_block; i++) {
    filter_time[i] = kernel[i] * scale;
  }
  free(kernel);

  resampler->input_fft = real_fft_create(two_n, err);
  resampler->output_fft = real_fft_create(2 * output_block, err);
  if (!resampler->input_fft || !resampler->output_fft) {
    free(filter_time);
    synchronous_resampler_free(resampler);
    return NULL;
  }

  // FFT the filter once at init; only the `inputBlock + 1` unique
  // bins are stored (real-input FFT has Hermitian symmetry, so the
  // upper half is redundant).
  resampler->filter_spec_re = (double*)calloc(input_block + 1, sizeof(double));
  resampler->filter_spec_im = (double*)calloc(input_block + 1, sizeof(double));
  if (!resampler->filter_spec_re || !resampler->filter_spec_im) {
    config_error_set(err, CONFIG_ERR_PARSE, "SynchronousResampler: Failed to allocate filter spectrum buffer");
    free(filter_time);
    synchronous_resampler_free(resampler);
    return NULL;
  }
  real_fft_forward(resampler->input_fft, filter_time, resampler->filter_spec_re,
                   resampler->filter_spec_im);
  free(filter_time);

  resampler->carries = (double**)calloc(channels, sizeof(double*));
  if (!resampler->carries) {
    config_error_set(err, CONFIG_ERR_PARSE, "SynchronousResampler: Failed to allocate channel carries array");
    synchronous_resampler_free(resampler);
    return NULL;
  }
  for (size_t ch = 0; ch < channels; ch++) {
    resampler->carries[ch] = (double*)calloc(output_block, sizeof(double));
    if (!resampler->carries[ch]) {
      config_error_set(err, CONFIG_ERR_PARSE, "SynchronousResampler: Failed to allocate carry buffer for channel %zu", ch);
      synchronous_resampler_free(resampler);
      return NULL;
    }
  }

  size_t max_len = input_block > output_block ? input_block : output_block;
  resampler->working_time = (double*)calloc(2 * max_len, sizeof(double));
  resampler->working_spec_re = (double*)calloc(max_len + 1, sizeof(double));
  resampler->working_spec_im = (double*)calloc(max_len + 1, sizeof(double));
  if (!resampler->working_time || !resampler->working_spec_re ||
      !resampler->working_spec_im) {
    config_error_set(err, CONFIG_ERR_PARSE, "SynchronousResampler: Failed to allocate working buffers");
    synchronous_resampler_free(resampler);
    return NULL;
  }

  return resampler;
}

void synchronous_resampler_free(synchronous_resampler_t* resampler) {
  if (!resampler) return;
  if (resampler->input_fft) real_fft_free(resampler->input_fft);
  if (resampler->output_fft) real_fft_free(resampler->output_fft);
  free(resampler->filter_spec_re);
  free(resampler->filter_spec_im);
  if (resampler->carries) {
    for (size_t ch = 0; ch < resampler->channels; ch++) {
      free(resampler->carries[ch]);
    }
    free(resampler->carries);
  }
  free(resampler->working_time);
  free(resampler->working_spec_re);
  free(resampler->working_spec_im);
  free(resampler);
}

/// `SynchronousResampler` runs at a fixed rational ratio fixed at
/// construction. The rate-adjust controller's relative multiplier
/// has nowhere to go here — we accept it without effect.
void synchronous_resampler_set_relative_ratio(
    synchronous_resampler_t* resampler, double multiplier) {
  (void)resampler;
  (void)multiplier;
  // Fixed-ratio
}

double synchronous_resampler_get_ratio(
    const synchronous_resampler_t* resampler) {
  return resampler ? resampler->ratio : 1.0;
}

size_t synchronous_resampler_get_max_output_frames(
    const synchronous_resampler_t* resampler) {
  return resampler ? resampler->output_chunk_size : 0;
}

size_t synchronous_resampler_get_chunk_size(
    const synchronous_resampler_t* resampler) {
  return resampler ? resampler->chunk_size : 0;
}

size_t synchronous_resampler_get_channels(
    const synchronous_resampler_t* resampler) {
  return resampler ? resampler->channels : 0;
}

/// One channel's worth of FFT-based overlap-add convolution +
/// spectral remap. All scratch buffers are class-owned; this
/// function performs no heap allocation.
resampler_error_t synchronous_resampler_process(
    synchronous_resampler_t* resampler, const audio_chunk_t* input,
    audio_chunk_t* output) {
  if (!resampler || !input || !output) return RESAMPLER_ERR_INVALID_PARAMETER;
  if (audio_chunk_get_valid_frames(input) != resampler->chunk_size) {
    return RESAMPLER_ERR_INPUT_SIZE_MISMATCH;
  }
  if (audio_chunk_get_channels(input) != resampler->channels) {
    return RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  if (audio_chunk_get_channels(output) != resampler->channels) {
    return RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  if (audio_chunk_get_frames(output) < resampler->output_chunk_size) {
    return RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL;
  }

  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* src_ptr = audio_chunk_get_channel(input, ch);
    double* out_ptr = audio_chunk_get_channel(output, ch);
    double* carry_ptr = resampler->carries[ch];
    if (!src_ptr || !out_ptr || !carry_ptr) continue;

    // Step 1. Place the input block at the start of a length-2N
    // buffer, with the second half zero. The zero-pad is what makes
    // the cyclic FFT convolution behave as a linear convolution
    // (Oppenheim & Schafer §8.7). The upper half is cleared each call
    // to ensure the zero-pad region for the forward FFT is clean.
    memcpy(resampler->working_time, src_ptr,
           resampler->input_block_len * sizeof(double));
    memset(resampler->working_time + resampler->input_block_len, 0,
           resampler->input_block_len * sizeof(double));

    // Step 2. Forward 2N-point real FFT.
    real_fft_forward(resampler->input_fft, resampler->working_time,
                     resampler->working_spec_re, resampler->working_spec_im);

    // Step 3. Pointwise multiply input spectrum by the pre-FFT'd
    // filter. Only the `sharedBins` matter since bins above are
    // dropped on the output side; doing the multiply in place over
    // that span avoids touching the upper half.
#ifdef ENABLE_ACCELERATE
    DSPDoubleSplitComplex io_split = {resampler->working_spec_re,
                                      resampler->working_spec_im};
    DSPDoubleSplitComplex f_split = {resampler->filter_spec_re,
                                     resampler->filter_spec_im};
    vDSP_zvmulD(&io_split, 1, &f_split, 1, &io_split, 1, resampler->shared_bins,
                1);
#else
    for (size_t i = 0; i < resampler->shared_bins; i++) {
      double re = resampler->working_spec_re[i];
      double im = resampler->working_spec_im[i];
      double fre = resampler->filter_spec_re[i];
      double fim = resampler->filter_spec_im[i];
      resampler->working_spec_re[i] = re * fre - im * fim;
      resampler->working_spec_im[i] = re * fim + im * fre;
    }
#endif

    // Step 4. Build the output spectrum of length `2·outputBlockLen`:
    // copy the filtered low bins and zero the rest. For upsampling
    // (outputBlockLen > inputBlockLen) the zeros above input Nyquist
    // are the spectral zero-pad that extends the bandwidth. For
    // downsampling they discard everything above output Nyquist —
    // the band-limiting step.
    if (resampler->output_block_len + 1 > resampler->shared_bins) {
      size_t zero_count =
          resampler->output_block_len + 1 - resampler->shared_bins;
      memset(resampler->working_spec_re + resampler->shared_bins, 0,
             zero_count * sizeof(double));
      memset(resampler->working_spec_im + resampler->shared_bins, 0,
             zero_count * sizeof(double));
    }

    // Step 5. Inverse 2P-point real FFT to time domain (P = outputBlockLen).
    real_fft_inverse(resampler->output_fft, resampler->working_spec_re,
                     resampler->working_spec_im, resampler->working_time);

    // Step 6. Overlap-add: write `result[0..P) + carry` as the chunk's
    // output samples, and save `result[P..2P)` for the next chunk's
    // overlap.
#ifdef ENABLE_ACCELERATE
    vDSP_vaddD(resampler->working_time, 1, carry_ptr, 1, out_ptr, 1,
               resampler->output_block_len);
#else
    for (size_t i = 0; i < resampler->output_block_len; i++) {
      out_ptr[i] = resampler->working_time[i] + carry_ptr[i];
    }
#endif
    memcpy(carry_ptr, resampler->working_time + resampler->output_block_len,
           resampler->output_block_len * sizeof(double));
  }

  audio_chunk_set_valid_frames(output, resampler->output_chunk_size);
  return RESAMPLER_OK;
}
