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
#include "Audio/prc_fmt.h"

#ifdef __cplusplus
extern "C" {
#endif

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

double sinc_dot_product_fn(const double* wave, const double* kernel, size_t count);

#ifdef __cplusplus
}
#endif

#endif // CLIB_RESAMPLER_SINC_DOT_PRODUCT_H
