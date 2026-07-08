/**
 * @file sinc_dot_product.h
 * @brief Dot product calculations for the windowed-sinc resampler.
 *
 * Inlined and regular functions to perform dot product calculations used
 * by the windowed-sinc resampler inner loop. Optimized for compiler
 * vectorization.
 */

#ifndef CLIB_RESAMPLER_SINC_DOT_PRODUCT_H
#define CLIB_RESAMPLER_SINC_DOT_PRODUCT_H

#include <stddef.h>

#include "Audio/double_helpers.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=fast", "associative-math")
#endif

/**
 * @brief Computes the dot product of a wave buffer and a sinc kernel.
 *
 * This function calculates the sum of the element-wise multiplication of the
 * input wave array and the sinc kernel array. It is inlined for performance
 * and optimized to allow Clang/LLVM to automatically vectorize the loop.
 *
 * @param wave Pointer to the input audio samples buffer.
 * @param kernel Pointer to the sinc filter kernel coefficient buffer.
 * @param count The number of elements to process (sinc length).
 * @return The resulting dot product (accumulated sum).
 */
static inline double sinc_dot_product(const double* wave, const double* kernel,
                                      size_t count) {
#if defined(__clang__)
#pragma clang fp reassociate(on) contract(fast)
#endif
  double sum = 0.0;
  for (size_t i = 0; i < count; i++) {
    sum += wave[i] * kernel[i];
  }
  return sum;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

/**
 * @brief Non-inlined version of the sinc dot product.
 *
 * Provides a standard function pointer target or non-inlined equivalent of
 * @ref sinc_dot_product.
 *
 * @param wave Pointer to the input audio samples buffer.
 * @param kernel Pointer to the sinc filter kernel coefficient buffer.
 * @param count The number of elements to process.
 * @return The resulting dot product (accumulated sum).
 */
double sinc_dot_product_fn(const double* wave, const double* kernel,
                           size_t count);

#endif  // CLIB_RESAMPLER_SINC_DOT_PRODUCT_H
