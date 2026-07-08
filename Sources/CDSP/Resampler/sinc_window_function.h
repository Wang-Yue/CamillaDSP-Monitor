/**
 * @file sinc_window_function.h
 * @brief Window functions and cutoff frequency calculations for the
 * windowed-sinc resampler kernel.
 */

#ifndef CLIB_RESAMPLER_SINC_WINDOW_FUNCTION_H
#define CLIB_RESAMPLER_SINC_WINDOW_FUNCTION_H

#include <stddef.h>

#include "Audio/double_helpers.h"

/**
 * @brief Window functions usable for sinc-filter kernel design.
 *
 * The `*2` variants represent the squared versions of the periodic base window.
 * Squaring the window function results in a wider main lobe but provides
 * stronger stopband attenuation.
 */
typedef enum {
  /** Hann window. */
  WINDOW_FUNCTION_HANN = 0,
  /** Squared Hann window. */
  WINDOW_FUNCTION_HANN2,
  /** Blackman window. */
  WINDOW_FUNCTION_BLACKMAN,
  /** Squared Blackman window. */
  WINDOW_FUNCTION_BLACKMAN2,
  /** Blackman-Harris window. */
  WINDOW_FUNCTION_BLACKMAN_HARRIS,
  /** Squared Blackman-Harris window. */
  WINDOW_FUNCTION_BLACKMAN_HARRIS2
} window_function_t;

/**
 * @brief Parses a window function from its string representation.
 *
 * @param str The string name of the window function (e.g., "Hann", "Blackman").
 * @param default_val The default value to return if the string does not match
 * any known window function.
 * @return The corresponding window_function_t value, or default_val.
 */
window_function_t window_function_from_string(const char* str,
                                              window_function_t default_val);

/**
 * @brief Converts a window function enum value to its string representation.
 *
 * @param wf The window function enum value.
 * @return A static string representing the window function.
 */
const char* window_function_to_string(window_function_t wf);

/**
 * @brief Calculates the value of a periodic window function at a specific
 * sample index.
 *
 * Each harmonic is computed in a specific order: `cos(2k * pi * i / n)`
 * computed as `((2k * pi) * i) / n` to maintain consistency and bit-equivalence
 * with Swift's implementation.
 *
 * @param window The window function to evaluate.
 * @param i The sample index.
 * @param n The total length of the window.
 * @return The window value at index `i`.
 */
double window_value(window_function_t window, size_t i, size_t n);

/**
 * @brief Calculates the cutoff frequency using 32-bit floating point
 * arithmetic.
 *
 * Although the main audio path runs in f64 (double precision), the cutoff
 * frequency calculations match f32 calculations for bit-equivalence with
 * kernel-derived constants in other implementations.
 *
 * @param sinc_len The length of the sinc filter.
 * @param window The window function used.
 * @return The cutoff frequency as a float.
 */
float calculate_cutoff_f32(size_t sinc_len, window_function_t window);

/**
 * @brief Calculates a suitable relative cutoff frequency for the given sinc
 * length and window.
 *
 * This function uses a cubic fit `1 / (k1/n + k2/n^2 + k3/n^3 + 1)` calibrated
 * per window.
 *
 * @param sinc_len The length of the sinc filter.
 * @param window The window function used.
 * @return The cutoff frequency as a double.
 */
double calculate_cutoff(size_t sinc_len, window_function_t window);

/**
 * @brief Builds the windowed-sinc table.
 *
 * The table is constructed by:
 * 1. Computing `y[i] = window[i] * sinc((i - totpoints/2) * fc / factor)` for
 *    i in `[0, totpoints)` using the periodic window.
 * 2. Summing y and dividing by `factor`.
 * 3. Decimating: `sincs[factor - n - 1][p] = y[factor*p + n] / norm`.
 * Stored layout: `table[s * sincLen + p] == sincs[s][p]`.
 *
 * The caller is responsible for freeing the returned pointer.
 *
 * @param sinc_len The length of each sinc filter (kernel length).
 * @param oversampling_factor The oversampling factor.
 * @param window The window function to apply.
 * @param fc The cutoff frequency (normalized).
 * @return A pointer to the newly allocated sinc table, or NULL on failure.
 */
double* make_sinc_table(size_t sinc_len, size_t oversampling_factor,
                        window_function_t window, double fc);

#endif  // CLIB_RESAMPLER_SINC_WINDOW_FUNCTION_H
