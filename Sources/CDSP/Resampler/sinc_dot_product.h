// Inlined dot product used by the windowed-sinc resampler inner loop.
//
// In C, we do not need SIMD2<Double> or unaligned load helpers (which were
// required in Swift to work around swiftc scalarization bugs).
// By unrolling 8 independent double accumulators (a0 through a7), Clang/LLVM
// under -O3 -ffp-contract=fast -fvectorize automatically maps the accumulators
// to 128-bit NEON vector registers (q0..q3) and emits vector loads (ldp) and
// Fused Multiply-Add instructions (fmla.2d).

#ifndef CLIB_RESAMPLER_SINC_DOT_PRODUCT_H
#define CLIB_RESAMPLER_SINC_DOT_PRODUCT_H

#include <stddef.h>
#include "Audio/double_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __APPLE__
static inline double sinc_dot_product(const double* wave, const double* kernel, size_t count) {
#if defined(__clang__)
#pragma clang fp reassociate(on) contract(fast)
#endif
    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum += wave[i] * kernel[i];
    }
    return sum;
}
#elif defined(__linux__)
static inline double sinc_dot_product(const double* wave, const double* kernel, size_t count) {
    double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    double a4 = 0.0, a5 = 0.0, a6 = 0.0, a7 = 0.0;

    size_t i = 0;
    size_t count_unrolled = count & ~7ULL;
    for (; i < count_unrolled; i += 8) {
        a0 += wave[i]     * kernel[i];
        a1 += wave[i + 1] * kernel[i + 1];
        a2 += wave[i + 2] * kernel[i + 2];
        a3 += wave[i + 3] * kernel[i + 3];
        a4 += wave[i + 4] * kernel[i + 4];
        a5 += wave[i + 5] * kernel[i + 5];
        a6 += wave[i + 6] * kernel[i + 6];
        a7 += wave[i + 7] * kernel[i + 7];
    }
    double sum = (a0 + a1) + (a2 + a3) + (a4 + a5) + (a6 + a7);
    for (; i < count; i++) {
        sum += wave[i] * kernel[i];
    }
    return sum;
}
#else
static inline double sinc_dot_product(const double* wave, const double* kernel, size_t count) {
    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum += wave[i] * kernel[i];
    }
    return sum;
}
#endif

double sinc_dot_product_fn(const double* wave, const double* kernel, size_t count);

#ifdef __cplusplus
}
#endif

#endif // CLIB_RESAMPLER_SINC_DOT_PRODUCT_H
