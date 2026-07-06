// Inlined dot product used by the windowed-sinc resampler
// inner loop.
//
// Optimized using explicit Swift SIMD2<Double> (mapping to ARM64 NEON
// float64x2_t) with 8 independent vector accumulators.
// Uses the project's `ldSIMD2` helper for unaligned vector loads.

import Foundation

@inline(__always)
func sincDotProduct(
  _ wave: UnsafePointer<Double>,
  _ kernel: UnsafePointer<Double>,
  _ count: Int
) -> Double {
  var acc0 = SIMD2<Double>(repeating: 0.0)
  var acc1 = SIMD2<Double>(repeating: 0.0)
  var acc2 = SIMD2<Double>(repeating: 0.0)
  var acc3 = SIMD2<Double>(repeating: 0.0)
  var acc4 = SIMD2<Double>(repeating: 0.0)
  var acc5 = SIMD2<Double>(repeating: 0.0)
  var acc6 = SIMD2<Double>(repeating: 0.0)
  var acc7 = SIMD2<Double>(repeating: 0.0)

  var idx = 0

  // 1. Loop unrolled by 16 elements (8 vectors of 2 Doubles)
  let unrolledEnd = count & ~15
  while idx < unrolledEnd {
    let w0 = ldSIMD2(wave, idx)
    let w1 = ldSIMD2(wave, idx + 2)
    let w2 = ldSIMD2(wave, idx + 4)
    let w3 = ldSIMD2(wave, idx + 6)
    let w4 = ldSIMD2(wave, idx + 8)
    let w5 = ldSIMD2(wave, idx + 10)
    let w6 = ldSIMD2(wave, idx + 12)
    let w7 = ldSIMD2(wave, idx + 14)

    let k0 = ldSIMD2(kernel, idx)
    let k1 = ldSIMD2(kernel, idx + 2)
    let k2 = ldSIMD2(kernel, idx + 4)
    let k3 = ldSIMD2(kernel, idx + 6)
    let k4 = ldSIMD2(kernel, idx + 8)
    let k5 = ldSIMD2(kernel, idx + 10)
    let k6 = ldSIMD2(kernel, idx + 12)
    let k7 = ldSIMD2(kernel, idx + 14)

    acc0 += w0 * k0
    acc1 += w1 * k1
    acc2 += w2 * k2
    acc3 += w3 * k3
    acc4 += w4 * k4
    acc5 += w5 * k5
    acc6 += w6 * k6
    acc7 += w7 * k7

    idx += 16
  }

  // 2. Loop for remaining pairs (2 elements at a time)
  let pairEnd = count & ~1
  while idx < pairEnd {
    let w0 = ldSIMD2(wave, idx)
    let k0 = ldSIMD2(kernel, idx)
    acc0 += w0 * k0
    idx += 2
  }

  // 3. Vector reduction
  let s01 = acc0 + acc1
  let s23 = acc2 + acc3
  let s45 = acc4 + acc5
  let s67 = acc6 + acc7
  let packedSum = (s01 + s23) + (s45 + s67)

  var sum = packedSum[0] + packedSum[1]

  // 4. Clean up the odd single tail element if count is odd
  if idx < count {
    sum += wave[idx] * kernel[idx]
  }

  return sum
}
