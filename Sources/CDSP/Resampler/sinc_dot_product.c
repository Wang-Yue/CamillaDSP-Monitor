// Inlined dot product used by the windowed-sinc resampler inner loop.
//
// Relies on Clang/LLVM auto-vectorization of 8 independent double accumulators
// into NEON 128-bit vector loads and Fused Multiply-Add instructions.

#include "sinc_dot_product.h"

/// Inlined dot product function wrapper for external callers.
double sinc_dot_product_fn(const double* wave, const double* kernel,
                           size_t count) {
  return sinc_dot_product(wave, kernel, count);
}
