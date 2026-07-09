/**
 * @file float_helpers.h
 * @brief DSP helper functions and vectorized operations using float precision.
 */

#ifndef CLIB_AUDIO_FLOAT_HELPERS_H
#define CLIB_AUDIO_FLOAT_HELPERS_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#if defined(ENABLE_ACCELERATE)
#include <Accelerate/Accelerate.h>
#elif defined(ENABLE_BLAS)
#include <cblas.h>
#include <string.h>
#else
#include <string.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief Multiply two float vectors element-wise.
 */
static inline void dsp_ops_float_multiply(const float* a, const float* b,
                                          float* result, size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vmul(a, 1, b, 1, result, 1, count);
#else
  for (size_t i = 0; i < count; i++) {
    result[i] = a[i] * b[i];
  }
#endif
}

/**
 * @brief Multiply float vector by scalar.
 */
static inline void dsp_ops_float_scalar_multiply(const float* vector,
                                                 float scalar, float* result,
                                                 size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vsmul(vector, 1, &scalar, result, 1, count);
#elif defined(ENABLE_BLAS)
  if (result == vector) {
    cblas_sscal((int)count, scalar, (float*)vector, 1);
  } else {
    memcpy(result, vector, count * sizeof(float));
    cblas_sscal((int)count, scalar, result, 1);
  }
#else
  for (size_t i = 0; i < count; i++) {
    result[i] = vector[i] * scalar;
  }
#endif
}

/**
 * @brief Generate a Hann window.
 */
static inline void dsp_ops_float_hann_window(float* buffer, size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_hann_window(buffer, (vDSP_Length)count, 0);
#else
  for (size_t i = 0; i < count; i++) {
    buffer[i] =
        0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)count));
  }
#endif
}

/**
 * @brief Find the maximum value in a float vector.
 */
static inline float dsp_ops_float_max(const float* buffer, size_t count) {
  if (count == 0) return -200.0f;
#if defined(ENABLE_ACCELERATE)
  float res = 0.0f;
  vDSP_maxv(buffer, 1, &res, count);
  return res;
#else
  float res = buffer[0];
  for (size_t i = 1; i < count; i++) {
    if (buffer[i] > res) res = buffer[i];
  }
  return res;
#endif
}

/**
 * @brief Compute the absolute magnitude of separate real and imaginary arrays.
 */
static inline void dsp_ops_float_zvabs(const float* real, const float* imag,
                                       float* magnitudes, size_t count) {
#if defined(ENABLE_ACCELERATE)
  DSPSplitComplex split = {(float*)real, (float*)imag};
  vDSP_zvabs(&split, 1, magnitudes, 1, count);
#else
  for (size_t i = 0; i < count; i++) {
    float re = real[i];
    float im = imag[i];
    magnitudes[i] = sqrtf(re * re + im * im);
  }
#endif
}

/**
 * @brief Threshold a float vector in-place or into another vector.
 */
static inline void dsp_ops_float_vthr(const float* vector, float threshold,
                                      float* result, size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vthr(vector, 1, &threshold, result, 1, count);
#else
  for (size_t i = 0; i < count; i++) {
    float val = vector[i];
    result[i] = val < threshold ? threshold : val;
  }
#endif
}

/**
 * @brief Convert linear amplitude values to decibels (dBFS).
 */
static inline void dsp_ops_float_vdbcon(const float* vector, float reference,
                                        float* result, size_t count) {
#if defined(ENABLE_ACCELERATE)
  vDSP_vdbcon(vector, 1, &reference, result, 1, count, 1);
#else
  for (size_t i = 0; i < count; i++) {
    float val = vector[i];
    if (val <= 0.0f) {
      result[i] = -200.0f;  // Limit minimum dB value to prevent log10(0) issues
    } else {
      result[i] = 20.0f * log10f(val / reference);
    }
  }
#endif
}

#endif  // CLIB_AUDIO_FLOAT_HELPERS_H
