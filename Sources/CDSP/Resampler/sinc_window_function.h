// Window functions + cutoff calculation for the windowed-sinc
// resampler kernel.

#ifndef CLIB_RESAMPLER_SINC_WINDOW_FUNCTION_H
#define CLIB_RESAMPLER_SINC_WINDOW_FUNCTION_H

#include <stddef.h>

#include "Audio/double_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Window functions usable for sinc-kernel design. The `*2` variants are the
/// squared versions of the periodic base window — wider main lobe but stronger
/// stopband attenuation.
typedef enum {
  WINDOW_FUNCTION_HANN = 0,
  WINDOW_FUNCTION_HANN2,
  WINDOW_FUNCTION_BLACKMAN,
  WINDOW_FUNCTION_BLACKMAN2,
  WINDOW_FUNCTION_BLACKMAN_HARRIS,
  WINDOW_FUNCTION_BLACKMAN_HARRIS2
} window_function_t;

window_function_t window_function_from_string(const char* str,
                                              window_function_t default_val);
const char* window_function_to_string(window_function_t wf);

/// Periodic window value at sample index `i` of a length-`n` window.
/// Mirrors `windowfunctions::GenericWindowIter::calc_at_index` — each harmonic
/// is `cos(2k · π · i / n)` computed with the operand order
/// `((2k * π) * i) / n`, **not** chained off the first harmonic. Reproducing
/// in this order.
double window_value(window_function_t window, size_t i, size_t n);

/// f32 cutoff calculation. The
/// audio path runs in f64 but the cutoff is computed in f32 and then
/// coerced up; we match that here so kernel-derived constants stay
/// bit-equivalent across resamplers.
float calculate_cutoff_f32(size_t sinc_len, window_function_t window);

/// Calculate a suitable relative cutoff frequency for the given sinc length and
/// window — a cubic
/// fit `1 / (k1/n + k2/n² + k3/n³ + 1)` calibrated per window.
double calculate_cutoff(size_t sinc_len, window_function_t window);

/// Build the windowed-sinc table:
///   1. Compute `y[i] = window[i] * sinc((i - totpoints/2) * fc / factor)` for
///      i ∈ [0, totpoints) using the periodic window.
///   2. Sum y, divide by `factor`.
///   3. Decimate: `sincs[factor - n - 1][p] = y[factor*p + n] / norm`.
/// Stored layout: `table[s * sincLen + p] == sincs[s][p]`.
double* make_sinc_table(size_t sinc_len, size_t oversampling_factor,
                        window_function_t window, double fc);

#ifdef __cplusplus
}
#endif

#endif  // CLIB_RESAMPLER_SINC_WINDOW_FUNCTION_H
