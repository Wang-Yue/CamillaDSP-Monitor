/**
 * @file double_helpers.h
 * @brief DSP helper functions and vectorized operations using double precision.
 *
 * Default is Double (f64). Change to Float for 32-bit processing.
 */

#ifndef CLIB_AUDIO_DOUBLE_HELPERS_H
#define CLIB_AUDIO_DOUBLE_HELPERS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(ENABLE_ACCELERATE)
#include <Accelerate/Accelerate.h>
#elif defined(ENABLE_BLAS)
#include <cblas.h>
#include <string.h>
#else
#include <string.h>
#endif

/**
 * @typedef mutable_waveform_t
 * @brief A high-performance descriptive view of a single channel's mutable
 * buffer pointer.
 */
typedef double* mutable_waveform_t;

/**
 * @typedef waveform_t
 * @brief A high-performance descriptive view of a single channel's buffer
 * pointer.
 */
typedef const double* waveform_t;

/**
 * @brief Convert dB to linear gain.
 *
 * @param db Value in decibels.
 * @return Linear gain.
 */
static inline double double_from_db(double db) { return pow(10.0, db / 20.0); }

/**
 * @brief Convert linear gain to dB.
 *
 * @param linear Linear gain value.
 * @return Value in decibels. Returns -1000.0 for zero/negative input.
 */
static inline double double_to_db(double linear) {
  if (linear <= 0.0) return -1000.0;
  return 20.0 * log10(linear);
}

/**
 * @brief Apply attack/release envelope smoothing to an input signal.
 *
 * @param input The current input value.
 * @param prev The smoothed value from the previous step.
 * @param attack_coeff The attack time constant coefficient.
 * @param release_coeff The release time constant coefficient.
 * @return The smoothed envelope value.
 */
static inline double double_smooth_envelope(double input, double prev,
                                            double attack_coeff,
                                            double release_coeff) {
  if (input >= prev) {
    return attack_coeff * prev + (1.0 - attack_coeff) * input;
  } else {
    return release_coeff * prev + (1.0 - release_coeff) * input;
  }
}

/**
 * @brief Computes modified Bessel function I0(x) using power series.
 *
 * Used for Kaiser window calculation.
 *
 * @param x Input value.
 * @return Value of I0(x).
 */
static inline double double_bessel_i0(double x) {
  double sum = 1.0;
  double denominator = 1.0;
  double i = 1.0;
  while (i < 25.0) {
    denominator *= i;
    double term = pow(x / 2.0, i) / denominator;
    sum += term * term;
    i += 1.0;
  }
  return sum;
}

// Vectorized DSP operations using Apple Accelerate (vDSP) or fallback C loops.
//
// The partial-count ops (add, multiply, multiply_add) need to
// operate on the first count elements of buffers that may be longer
// (chunks have a valid_frames <= frames).
//
// In C, these accept pointers directly so callers holding stable pointers
// (e.g. an audio_buffers_t channel view) avoid any copy overhead or
// ownership checks on the audio thread.

/**
 * @brief Multiply vector by scalar in-place.
 *
 * Computes: `buffer[i] *= scalar` for `i < count`.
 *
 * @param buffer The buffer to multiply (in-place).
 * @param scalar The scalar multiplier.
 * @param count Number of elements to process.
 */
static inline void dsp_ops_scalar_multiply(mutable_waveform_t buffer,
                                           double scalar, size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vsmulD(buffer, 1, &scalar, buffer, 1, count);
#elif defined(ENABLE_BLAS)
  cblas_dscal((int)count, scalar, buffer, 1);
#else
  for (size_t i = 0; i < count; i++) {
    buffer[i] *= scalar;
  }
#endif
}

/**
 * @brief Zero `count` samples in-place.
 *
 * @param buffer The buffer to clear.
 * @param count Number of elements to clear.
 */
static inline void dsp_ops_clear(mutable_waveform_t buffer, size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vclrD(buffer, 1, count);
#elif defined(ENABLE_BLAS)
  memset(buffer, 0, count * sizeof(double));
#else
  for (size_t i = 0; i < count; i++) {
    buffer[i] = 0.0;
  }
#endif
}

/**
 * @brief Add vector `a` to vector `b` in-place (on `b`).
 *
 * Computes: `b[i] += a[i]` for `i < count`.
 * Must satisfy `count <= capacity(a)` and `count <= capacity(b)`.
 *
 * @param a Input vector.
 * @param b Destination vector (modified in-place).
 * @param count Number of elements to process.
 */
static inline void dsp_ops_add(waveform_t a, mutable_waveform_t b,
                               size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vaddD(a, 1, b, 1, b, 1, count);
#elif defined(ENABLE_BLAS)
  cblas_daxpy((int)count, 1.0, a, 1, b, 1);
#else
  for (size_t i = 0; i < count; i++) {
    b[i] += a[i];
  }
#endif
}

/**
 * @brief Multiply two vectors element-wise.
 *
 * Computes: `b[i] *= a[i]` for `i < count` (in-place on `b`).
 *
 * @param a Input vector.
 * @param b Destination vector (modified in-place).
 * @param count Number of elements to process.
 */
static inline void dsp_ops_multiply(waveform_t a, mutable_waveform_t b,
                                    size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vmulD(a, 1, b, 1, b, 1, count);
#else
  for (size_t i = 0; i < count; i++) {
    b[i] *= a[i];
  }
#endif
}

/**
 * @brief Multiply-accumulate: `accumulator[i] += a[i] * scalar` for `i <
 * count`.
 *
 * @param a Input vector.
 * @param scalar The scalar multiplier.
 * @param accumulator The accumulator vector (modified in-place).
 * @param count Number of elements to process.
 */
static inline void dsp_ops_multiply_add(waveform_t a, double scalar,
                                        mutable_waveform_t accumulator,
                                        size_t count) {
#if defined(ENABLE_ACCELERATE)
  // result = (a * scalar) + accumulator, written into accumulator.
  vDSP_vsmaD(a, 1, &scalar, accumulator, 1, accumulator, 1, count);
#elif defined(ENABLE_BLAS)
  cblas_daxpy((int)count, scalar, a, 1, accumulator, 1);
#else
  for (size_t i = 0; i < count; i++) {
    accumulator[i] += a[i] * scalar;
  }
#endif
}

/**
 * @brief Find peak absolute value across the first `count` samples of the
 * buffer.
 *
 * @param buffer Input vector.
 * @param count Number of elements to process.
 * @return The peak absolute value, or 0.0 if count is 0.
 */
static inline double dsp_ops_peak_absolute(waveform_t buffer, size_t count) {
  if (count == 0) return 0.0;
#if defined(ENABLE_ACCELERATE)
  double res = 0.0;
  vDSP_maxmgvD(buffer, 1, &res, count);
  return res;
#elif defined(ENABLE_BLAS)
  int idx = cblas_idamax((int)count, buffer, 1);
  return fabs(buffer[idx]);
#else
  double res = 0.0;
  for (size_t i = 0; i < count; i++) {
    double val = fabs(buffer[i]);
    if (val > res) res = val;
  }
  return res;
#endif
}

/**
 * @brief Compute root-mean-square over the first `count` samples of the buffer.
 *
 * @param buffer Input vector.
 * @param count Number of elements to process.
 * @return The RMS value, or 0.0 if count is 0.
 */
static inline double dsp_ops_rms(waveform_t buffer, size_t count) {
  if (count == 0) return 0.0;
#if defined(ENABLE_ACCELERATE)
  double res = 0.0;
  vDSP_rmsqvD(buffer, 1, &res, count);
  return res;
#elif defined(ENABLE_BLAS)
  double norm = cblas_dnrm2((int)count, buffer, 1);
  return norm / sqrt((double)count);
#else
  double sum = 0.0;
  for (size_t i = 0; i < count; i++) {
    sum += buffer[i] * buffer[i];
  }
  return sqrt(sum / count);
#endif
}

#endif  // CLIB_AUDIO_DOUBLE_HELPERS_H
