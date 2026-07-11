/**
 * @file synchronous_resampler.h
 * @brief FFT-based fixed-ratio sample-rate converter.
 *
 * Independently derived from textbook descriptions of FFT-based rate
 * conversion via overlap-add convolution and spectral resampling.
 *
 * References
 * ----------
 *   * R. E. Crochiere and L. R. Rabiner (1983), "Multirate Digital
 *     Signal Processing", Prentice-Hall — §3 covers the L/M
 *     decimator-interpolator structure and its block-rate FFT
 *     realisation.
 *   * A. V. Oppenheim and R. W. Schafer, "Discrete-Time Signal
 *     Processing", Prentice-Hall — §4 (sample-rate alteration), §8.7
 *     ("Overlap-Save and Overlap-Add Methods" for FFT-based linear
 *     convolution), §8.8 (FFT-based fast convolution).
 *   * J. O. Smith, "Digital Audio Resampling Home Page", CCRMA —
 *     https://ccrma.stanford.edu/~jos/resample/ — covers FFT-based
 *     bandlimited interpolation and windowed-sinc filter design.
 *   * F. J. Harris (1978), "On the Use of Windows for Harmonic
 *     Analysis with the Discrete Fourier Transform", Proc. IEEE
 *     vol. 66 no. 1 — Blackman-Harris window (used here via
 *     `SincWindowFunction.swift`).
 *
 * Algorithm
 * ---------
 * Given input rate `Fᵢ`, output rate `Fₒ`, and `g = gcd(Fᵢ, Fₒ)`,
 * define
 *
 *     L = Fᵢ / g     (input block size in samples per rational period)
 *     M = Fₒ / g     (output block size in samples per rational period)
 *
 * Any integer multiple `N = K·L` input samples corresponds to
 * exactly `K·M` output samples — the resampler is fixed-ratio. We
 * round the user-supplied `chunkSize` up to the smallest valid
 * `K·L`, which fixes the per-call input/output block lengths.
 *
 * At init, build a windowed-sinc lowpass filter `h[n]` of length `N`
 * with cutoff at `min(1, Fₒ/Fᵢ) · π` rad/sample (Crochiere & Rabiner
 * §3.1, Smith CCRMA §"Windowed-Sinc Filter Design"), zero-pad to
 * length `2N`, and pre-FFT it once into `H`.
 *
 * Per chunk per channel:
 *
 *   1. Forward 2N-point real FFT of the zero-padded input. The
 *      zero-pad to length 2N converts the otherwise cyclic FFT
 *      convolution into a linear convolution — the standard
 *      overlap-add structure in Oppenheim & Schafer §8.7.
 *
 *   2. Multiply pointwise by `H` to apply the anti-aliasing filter
 *      in the frequency domain. Cost: O(N) per chunk versus O(N²)
 *      for a direct time-domain convolution.
 *
 *   3. Build the output spectrum of length `2P` where `P = K·M`:
 *        — bins 0…min(N, P) get a copy of the filtered input
 *          spectrum;
 *        — bins above are set to zero.
 *      For upsampling (M > L), the zero-pad above input Nyquist is
 *      what extends the bandwidth. For downsampling (M < L),
 *      truncating to the first `P + 1` unique bins is the
 *      band-limiting step. This is the "spectral resampling" of
 *      Smith's CCRMA notes.
 *
 *   4. Inverse 2P-point real FFT recovers a length-2P time-domain
 *      block.
 *
 *   5. Overlap-add: emit `result[0..P) + carry`, save
 *      `result[P..2P)` as `carry` for the next chunk
 *      (Oppenheim & Schafer §8.7).
 *
 * The arbitrary-length real FFTs are handled by `RealFFT`,
 * which lets the block lengths be sized exactly to `L` and `M`
 * rather than padded to a power of two.
 *
 * Allocation discipline
 * ---------------------
 * Every per-channel and per-call buffer is allocated once at
 * `init`. `process(input:into:)` does no heap allocation and writes
 * directly into the caller's pre-allocated `output` chunk.
 */

#ifndef CLIB_RESAMPLER_SYNCHRONOUS_RESAMPLER_H
#define CLIB_RESAMPLER_SYNCHRONOUS_RESAMPLER_H

#include <stdbool.h>
#include <stddef.h>

#include "Audio/audio_chunk.h"
#include "Config/config_error.h"
#include "FFT/real_fft.h"
#include "resampler_error.h"
#include "sinc_window_function.h"

/**
 * @brief Opaque structure representing the synchronous resampler state.
 */
struct synchronous_resampler;
typedef struct synchronous_resampler synchronous_resampler_t;

/**
 * @brief Creates and initializes a synchronous resampler instance.
 *
 * This function allocates and initializes the resources needed for resampling.
 * It determines the optimal internal block sizes based on the input and output
 * rates, and the requested chunk size. It also builds and FFT-transforms the
 * anti-aliasing sinc filter.
 *
 * @param channels The number of audio channels.
 * @param input_rate The input sample rate in Hz.
 * @param output_rate The output sample rate in Hz.
 * @param requested_chunk_size The desired size of input chunks (in frames).
 *                             The resampler will round this up to a size
 * matching the rational period.
 * @param err Pointer to a config error struct to populate on failure.
 * @return A pointer to the created resampler instance, or NULL on failure.
 */
synchronous_resampler_t* synchronous_resampler_create(
    size_t channels, size_t input_rate, size_t output_rate,
    size_t requested_chunk_size, config_error_t* err);

/**
 * @brief Frees all memory allocated for the synchronous resampler.
 *
 * @param resampler Pointer to the resampler instance to destroy.
 */
void synchronous_resampler_free(synchronous_resampler_t* resampler);

/**
 * @brief Processes a chunk of audio through the resampler.
 *
 * Performs overlap-add FFT convolution on each channel of the input audio
 * chunk, applies spectral resampling (zero-padding or truncation), and writes
 * the resampled audio to the output chunk.
 *
 * @param resampler Pointer to the resampler instance.
 * @param input Pointer to the input audio chunk.
 * @param output Pointer to the output audio chunk. The output buffer must
 *               be pre-allocated and large enough to hold the output frames
 *               (see @ref synchronous_resampler_get_max_output_frames).
 * @return A `resampler_error_t` code indicating success or failure.
 */
resampler_error_t synchronous_resampler_process(
    synchronous_resampler_t* resampler, const audio_chunk_t* input,
    audio_chunk_t* output);

/**
 * @brief Dynamically adjusts the resampling ratio by a relative multiplier.
 *
 * Allows fine-tuning of the resampler ratio (e.g. for drift compensation).
 *
 * @param resampler Pointer to the resampler instance.
 * @param multiplier The relative multiplier to apply to the nominal output
 * rate.
 */
void synchronous_resampler_set_relative_ratio(
    synchronous_resampler_t* resampler, double multiplier);

/**
 * @brief Gets the current actual resampling ratio (output rate / input rate).
 *
 * @param resampler Pointer to the resampler instance.
 * @return The current resampling ratio as a double.
 */
double synchronous_resampler_get_ratio(
    const synchronous_resampler_t* resampler);

/**
 * @brief Gets the maximum number of output frames that can be produced in a
 * single call.
 *
 * Useful for allocating the output buffer before calling @ref
 * synchronous_resampler_process.
 *
 * @param resampler Pointer to the resampler instance.
 * @return The maximum number of output frames.
 */
size_t synchronous_resampler_get_max_output_frames(
    const synchronous_resampler_t* resampler);

/**
 * @brief Gets the internal processing chunk size in input frames.
 *
 * @param resampler Pointer to the resampler instance.
 * @return The chunk size (in frames).
 */
size_t synchronous_resampler_get_chunk_size(
    const synchronous_resampler_t* resampler);

/**
 * @brief Gets the number of channels supported by the resampler.
 *
 * @param resampler Pointer to the resampler instance.
 * @return The channel count.
 */
size_t synchronous_resampler_get_channels(
    const synchronous_resampler_t* resampler);

#endif  // CLIB_RESAMPLER_SYNCHRONOUS_RESAMPLER_H
