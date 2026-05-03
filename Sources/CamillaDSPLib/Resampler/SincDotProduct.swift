// CamillaDSP-Swift: inlined dot product used by the windowed-sinc resampler
// inner loop.
//
// 8 independent accumulators give the optimiser enough independence to emit
// packed NEON `fmla` pairs on Apple Silicon. The final reduction
//   acc0 + acc1 + acc2 + acc3 + acc4 + acc5 + acc6 + acc7
// is left-associative — same as rubato's `ScalarInterpolator` at
// `sinc_interpolator/mod.rs:113`. Matching the reduction tree (rather than a
// balanced one) gives bit-equivalent ULP-level results versus Rust.

import Foundation

@inline(__always)
func sincDotProduct(
  _ wave: UnsafePointer<Double>,
  _ kernel: UnsafePointer<Double>,
  _ count: Int
) -> Double {
  var a0 = 0.0
  var a1 = 0.0
  var a2 = 0.0
  var a3 = 0.0
  var a4 = 0.0
  var a5 = 0.0
  var a6 = 0.0
  var a7 = 0.0
  var i = 0
  let unrolledEnd = count & ~7
  while i < unrolledEnd {
    a0 += wave[i] * kernel[i]
    a1 += wave[i &+ 1] * kernel[i &+ 1]
    a2 += wave[i &+ 2] * kernel[i &+ 2]
    a3 += wave[i &+ 3] * kernel[i &+ 3]
    a4 += wave[i &+ 4] * kernel[i &+ 4]
    a5 += wave[i &+ 5] * kernel[i &+ 5]
    a6 += wave[i &+ 6] * kernel[i &+ 6]
    a7 += wave[i &+ 7] * kernel[i &+ 7]
    i &+= 8
  }
  var tail = 0.0
  while i < count {
    tail += wave[i] * kernel[i]
    i &+= 1
  }
  // Left-associative reduction matching rubato exactly. The compiler can
  // still issue dependency-free per-lane FMAs above; this only constrains
  // the final 7-add chain.
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + tail
}
