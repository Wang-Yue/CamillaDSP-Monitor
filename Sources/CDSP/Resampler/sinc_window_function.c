// Window functions + cutoff calculation for the windowed-sinc
// resampler kernel.

#include "sinc_window_function.h"

#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

window_function_t window_function_from_string(const char* str,
                                              window_function_t default_val) {
  if (!str) return default_val;
  if (strcmp(str, "Hann") == 0) return WINDOW_FUNCTION_HANN;
  if (strcmp(str, "Hann2") == 0) return WINDOW_FUNCTION_HANN2;
  if (strcmp(str, "Blackman") == 0) return WINDOW_FUNCTION_BLACKMAN;
  if (strcmp(str, "Blackman2") == 0) return WINDOW_FUNCTION_BLACKMAN2;
  if (strcmp(str, "BlackmanHarris") == 0)
    return WINDOW_FUNCTION_BLACKMAN_HARRIS;
  if (strcmp(str, "BlackmanHarris2") == 0)
    return WINDOW_FUNCTION_BLACKMAN_HARRIS2;
  return default_val;
}

const char* window_function_to_string(window_function_t wf) {
  switch (wf) {
    case WINDOW_FUNCTION_HANN:
      return "Hann";
    case WINDOW_FUNCTION_HANN2:
      return "Hann2";
    case WINDOW_FUNCTION_BLACKMAN:
      return "Blackman";
    case WINDOW_FUNCTION_BLACKMAN2:
      return "Blackman2";
    case WINDOW_FUNCTION_BLACKMAN_HARRIS:
      return "BlackmanHarris";
    case WINDOW_FUNCTION_BLACKMAN_HARRIS2:
      return "BlackmanHarris2";
    default:
      return "Hann";
  }
}

/// Periodic window value at sample index `i` of a length-`n` window.
/// Mirrors `windowfunctions::GenericWindowIter::calc_at_index` — each harmonic
/// is `cos(2k · π · i / n)` computed with the operand order
/// `((2k * π) * i) / n`, **not** chained off the first harmonic. Reproducing
/// in this order.
double window_value(window_function_t window, size_t i, size_t n) {
  double x = (double)i;
  double len = (double)n;
  // Match `(2k * PI * x_float / len_float).cos()` from windowfunctions 0.1.1.
  double arg2 = 2.0 * M_PI * x / len;
  double arg4 = 4.0 * M_PI * x / len;
  double arg6 = 6.0 * M_PI * x / len;
  switch (window) {
    case WINDOW_FUNCTION_HANN:
      return 0.5 - 0.5 * cos(arg2);
    case WINDOW_FUNCTION_HANN2: {
      double w = 0.5 - 0.5 * cos(arg2);
      return w * w;
    }
    case WINDOW_FUNCTION_BLACKMAN:
      return 0.42 - 0.5 * cos(arg2) + 0.08 * cos(arg4);
    case WINDOW_FUNCTION_BLACKMAN2: {
      double w = 0.42 - 0.5 * cos(arg2) + 0.08 * cos(arg4);
      return w * w;
    }
    case WINDOW_FUNCTION_BLACKMAN_HARRIS:
      return 0.35875 - 0.48829 * cos(arg2) + 0.14128 * cos(arg4) -
             0.01168 * cos(arg6);
    case WINDOW_FUNCTION_BLACKMAN_HARRIS2: {
      double w = 0.35875 - 0.48829 * cos(arg2) + 0.14128 * cos(arg4) -
                 0.01168 * cos(arg6);
      return w * w;
    }
    default:
      return 1.0;
  }
}

/// f32 cutoff calculation. The
/// audio path runs in f64 but the cutoff is computed in f32 and then
/// coerced up; we match that here so kernel-derived constants stay
/// bit-equivalent across resamplers.
float calculate_cutoff_f32(size_t sinc_len, window_function_t window) {
  float k1 = 0.0f, k2 = 0.0f, k3 = 0.0f;
  switch (window) {
    case WINDOW_FUNCTION_BLACKMAN_HARRIS:
      k1 = 8.041443677716476f;
      k2 = 55.9506779343387f;
      k3 = 898.0287985384213f;
      break;
    case WINDOW_FUNCTION_BLACKMAN_HARRIS2:
      k1 = 13.745202940783823f;
      k2 = 121.73532586374934f;
      k3 = 5964.163279612051f;
      break;
    case WINDOW_FUNCTION_BLACKMAN:
      k1 = 6.159598046201173f;
      k2 = 18.926415097606878f;
      k3 = 653.4247430458968f;
      break;
    case WINDOW_FUNCTION_BLACKMAN2:
      k1 = 9.506235102129398f;
      k2 = 79.13120634953742f;
      k3 = 1502.2316160588925f;
      break;
    case WINDOW_FUNCTION_HANN:
      k1 = 3.3481080887677166f;
      k2 = 10.106519434875038f;
      k3 = 78.96345249024414f;
      break;
    case WINDOW_FUNCTION_HANN2:
      k1 = 5.38751148378734f;
      k2 = 29.69451915489501f;
      k3 = 184.82117462266237f;
      break;
    default:
      k1 = 3.3481080887677166f;
      k2 = 10.106519434875038f;
      k3 = 78.96345249024414f;
      break;
  }
  float n = (float)sinc_len;
  return 1.0f / (k1 / n + k2 / (n * n) + k3 / (n * n * n) + 1.0f);
}

/// Calculate a suitable relative cutoff frequency for the given sinc length and
/// window — a cubic
/// fit `1 / (k1/n + k2/n² + k3/n³ + 1)` calibrated per window.
double calculate_cutoff(size_t sinc_len, window_function_t window) {
  double k1 = 0.0, k2 = 0.0, k3 = 0.0;
  switch (window) {
    case WINDOW_FUNCTION_BLACKMAN_HARRIS:
      k1 = 8.041443677716476;
      k2 = 55.9506779343387;
      k3 = 898.0287985384213;
      break;
    case WINDOW_FUNCTION_BLACKMAN_HARRIS2:
      k1 = 13.745202940783823;
      k2 = 121.73532586374934;
      k3 = 5964.163279612051;
      break;
    case WINDOW_FUNCTION_BLACKMAN:
      k1 = 6.159598046201173;
      k2 = 18.926415097606878;
      k3 = 653.4247430458968;
      break;
    case WINDOW_FUNCTION_BLACKMAN2:
      k1 = 9.506235102129398;
      k2 = 79.13120634953742;
      k3 = 1502.2316160588925;
      break;
    case WINDOW_FUNCTION_HANN:
      k1 = 3.3481080887677166;
      k2 = 10.106519434875038;
      k3 = 78.96345249024414;
      break;
    case WINDOW_FUNCTION_HANN2:
      k1 = 5.38751148378734;
      k2 = 29.69451915489501;
      k3 = 184.82117462266237;
      break;
    default:
      k1 = 3.3481080887677166;
      k2 = 10.106519434875038;
      k3 = 78.96345249024414;
      break;
  }
  double n = (double)sinc_len;
  return 1.0 / (k1 / n + k2 / (n * n) + k3 / (n * n * n) + 1.0);
}

/// Build the windowed-sinc table:
///   1. Compute `y[i] = window[i] * sinc((i - totpoints/2) * fc / factor)` for
///      i ∈ [0, totpoints) using the periodic window.
///   2. Sum y, divide by `factor`.
///   3. Decimate: `sincs[factor - n - 1][p] = y[factor*p + n] / norm`.
/// Stored layout: `table[s * sincLen + p] == sincs[s][p]`.
double* make_sinc_table(size_t sinc_len, size_t oversampling_factor,
                        window_function_t window, double fc) {
  if (sinc_len > 0 && oversampling_factor > SIZE_MAX / sinc_len) {
    return NULL;
  }
  size_t totpoints = sinc_len * oversampling_factor;
  double* y = (double*)calloc(totpoints, sizeof(double));
  if (!y) return NULL;

  // Generate the high-resolution prototype sinc filter.
  // The filter is centered to ensure linear phase (symmetric kernel).
  for (size_t i = 0; i < totpoints; i++) {
    double centred = (double)i - (double)(totpoints / 2);
    // Scale the index by oversampling factor to evaluate the continuous sinc
    // function at the high-resolution fractional positions.
    double x_scaled = centred * fc / (double)oversampling_factor;
    // sinc(x) = (x * PI).sin() / (x * PI) — argument order
    // matters for the (rare) f64 ULP differences this avoids.
    double arg = x_scaled * M_PI;
    // Handle the division by zero at the center of the sinc.
    double sinc = (fabs(x_scaled) < 1e-10) ? 1.0 : sin(arg) / arg;
    y[i] = sinc * window_value(window, i, totpoints);
  }

  // Calculate the DC gain of the polyphase filter bank.
  // The sum of the prototype filter is divided by the oversampling factor
  // because the output of each sub-filter is effectively scaled by 1/factor.
  double y_sum = 0.0;
  for (size_t i = 0; i < totpoints; i++) {
    y_sum += y[i];
  }
  double norm = y_sum / (double)oversampling_factor;

  double* table = (double*)calloc(totpoints, sizeof(double));
  if (!table) {
    free(y);
    return NULL;
  }

  // Decimate the prototype filter into the polyphase sub-filters.
  // The table is structured as an array of 'oversampling_factor' sub-filters
  // (phases), each containing 'sinc_len' coefficients. The phases are stored in
  // reverse order (oversampling_factor - n - 1) to match the convolution
  // indexing behavior of the fractional delay resampler.
  for (size_t p = 0; p < sinc_len; p++) {
    for (size_t n = 0; n < oversampling_factor; n++) {
      size_t s = oversampling_factor - n - 1;
      table[s * sinc_len + p] = y[oversampling_factor * p + n] / norm;
    }
  }

  free(y);
  return table;
}
