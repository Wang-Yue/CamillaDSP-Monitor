// factors are all ≤ 7. Targets `N = 1029 = 3 · 7³` and `N = 1120 = 2⁵ · 5 · 7`
// — the inner FFT sizes that RealFFT needs for 44.1↔48 kHz
// resampling. Compared with Bluestein-on-vDSP, this trades the inner
// power-of-2 transforms (M = 4096) for a direct decomposition into
// `O(N · Σ pᵢ)` ops — about 6× fewer arithmetic operations at N = 1029.
//
// Note on the radix-2/4/8 stages: they're not redundant with
// `RealFFT`'s outer `vDSP_fft_zrip` fast path. That fast path
// fires only when the *whole* real-FFT length is a power of two; the
// radix-2/4/8 stages here handle the *power-of-two portion* of a mixed
// factorisation (e.g. `1120 = 2⁵·5·7` collapses into `[8, 4, 5, 7]`).
// Without them this class could only support odd-prime-only sizes like
// `105 = 3·5·7`, and most of our resampler's mixed-rate FFTs would fall
// through to Bluestein.
//
// Architecture: classic iterative DIT (decimation-in-time) Cooley-Tukey.
//   1. Permute input via mixed-radix digit reversal.
//   2. For each factor `r` (in order), apply length-`r` butterflies on
//      stride-`m` groups, where `m` grows by `r` after each stage. Twiddle
//      factors W_{m·r}^(j·k) are pre-computed once at init.
//   3. Copy out (with conjugation for the inverse direction).
//
// Inverse FFT uses the identity `IDFT(x) = conj(DFT(conj(x)))`, so we only
// pre-compute the forward twiddles. Both transforms are unnormalised.
//
// All buffers (twiddles, permutation LUT, scratch) are heap-allocated at
// init and freed in deinit. The hot path runs purely on raw pointers — no
// allocations, no closures.

import Foundation

/// Mixed-radix complex FFT supporting `N = 2^a · 3^b · 5^c · 7^d`. Returns
/// `nil` if `N` has any prime factor > 7 — caller should fall back to
/// Bluestein in that case.
final class MixedRadixFFT: ArbitraryComplexFFT {
  let n: Int
  /// Prime factorisation of `n`, smallest first. The DIT stages walk this
  /// list left-to-right.
  private let factors: [Int]
  private let stageCount: Int

  /// Per-stage forward twiddles, length `m_s · r_s` (for stage `s` with
  /// pre-stage subblock size `m_s` and radix `r_s`). The `j = 0` row is
  /// trivial (W^0 = 1) but we keep it for uniform indexing.
  private let twiddleRe: UnsafeMutablePointer<UnsafeMutablePointer<Double>>
  private let twiddleIm: UnsafeMutablePointer<UnsafeMutablePointer<Double>>

  /// Mixed-radix digit-reversal permutation. `permutation[i]` is where
  /// input element `i` ends up in the post-permutation buffer.
  private let permutation: UnsafeMutablePointer<Int>

  /// Active read/write buffers for the butterfly stages. Re-pointed at
  /// the caller's `realOut`/`imagOut` at the start of each `execute`
  /// call — the permutation step writes the post-permute samples
  /// directly into the output buffer, every stage runs in-place on
  /// the output, and we skip the final memcpy that the older "internal
  /// scratch + copy out" pattern needed.
  ///
  /// Only valid for the duration of one `execute` invocation. Aliasing
  /// `realIn` and `realOut` is unsupported (the permute pass would
  /// overwrite input bytes mid-pass).
  private var workRe: UnsafeMutablePointer<Double>!
  private var workIm: UnsafeMutablePointer<Double>!

  /// Constants used by the radix-5, 7 butterflies. Pre-computed for the
  /// forward direction; the inverse path conjugates the input/output, so
  /// the same constants work for both.

  // Radix-5 constants: tw_5^k = exp(-2πi·k/5).
  private static let c5_1Re: Double = cos(2.0 * .pi / 5.0)
  private static let c5_1Im: Double = -sin(2.0 * .pi / 5.0)
  private static let c5_2Re: Double = cos(4.0 * .pi / 5.0)
  private static let c5_2Im: Double = -sin(4.0 * .pi / 5.0)

  // Radix-7 constants: tw_7^k for k = 1..3 (k = 4..6 are conjugates).
  private static let c7_1Re: Double = cos(2.0 * .pi / 7.0)
  private static let c7_1Im: Double = -sin(2.0 * .pi / 7.0)
  private static let c7_2Re: Double = cos(4.0 * .pi / 7.0)
  private static let c7_2Im: Double = -sin(4.0 * .pi / 7.0)
  private static let c7_3Re: Double = cos(6.0 * .pi / 7.0)
  private static let c7_3Im: Double = -sin(6.0 * .pi / 7.0)

  init?(n: Int) {
    precondition(n > 0)
    // Factorise into 2/3/4/5/7/8 with the power-of-2 portion preferring
    // larger radixes — `2⁵ = 32 → [8, 4]` (2 stages) vs `[2, 2, 2, 2, 2]`
    // (5 stages). Each stage saved cuts a length-N twiddle multiply pass
    // and the loop-overhead that comes with it. For N = 1120 = 2⁵·5·7 this
    // collapses 7 stages to 4: `[8, 4, 5, 7]`.
    var fs: [Int] = []
    var rem = n
    var twoPow = 0
    while rem % 2 == 0 {
      twoPow += 1
      rem /= 2
    }
    // Greedy: take 8s while we have ≥ 3 powers of 2 remaining, then a
    // single 4 if 2 remain, or a 2 if 1 remains.
    while twoPow >= 3 {
      fs.append(8)
      twoPow -= 3
    }
    if twoPow == 2 {
      fs.append(4)
    } else if twoPow == 1 {
      fs.append(2)
    }
    for p in [3, 5, 7] {
      while rem % p == 0 {
        fs.append(p)
        rem /= p
      }
    }
    guard rem == 1 else { return nil }  // unsupported large prime
    self.n = n
    self.factors = fs
    self.stageCount = fs.count

    // Allocate per-stage twiddle buffers.
    let twReSlots = UnsafeMutablePointer<UnsafeMutablePointer<Double>>.allocate(capacity: fs.count)
    let twImSlots = UnsafeMutablePointer<UnsafeMutablePointer<Double>>.allocate(capacity: fs.count)
    var m = 1
    for (s, r) in fs.enumerated() {
      let len = m * r
      let re = UnsafeMutablePointer<Double>.allocate(capacity: len)
      let im = UnsafeMutablePointer<Double>.allocate(capacity: len)
      // twiddle[j*m + k] = W_{m·r}^(j·k) for j in 0..r-1, k in 0..m-1.
      let invMR = 1.0 / Double(m * r)
      for j in 0..<r {
        for k in 0..<m {
          let theta = -2.0 * .pi * Double(j * k) * invMR
          re[j * m + k] = cos(theta)
          im[j * m + k] = sin(theta)
        }
      }
      twReSlots[s] = re
      twImSlots[s] = im
      m *= r
    }
    self.twiddleRe = twReSlots
    self.twiddleIm = twImSlots

    // Pre-compute the digit-reversal permutation. We store `factors` in
    // stage-iteration order (`factors[0]` is the radix processed first, with
    // `m = 1`); the corresponding decimation order, used to build the perm,
    // is the reverse. So we iterate `factors.reversed()` here. Failing to
    // reverse leaves stage 0 operating on the wrong input groups — the bug
    // that turned this whole mixed-radix path into garbage on the first
    // attempt.
    let permPtr = UnsafeMutablePointer<Int>.allocate(capacity: n)
    let decimation = Array(fs.reversed())
    for i in 0..<n {
      var idx = i
      var rev = 0
      var mLeft = n
      for r in decimation {
        mLeft /= r
        let d = idx % r
        idx /= r
        rev += d * mLeft
      }
      permPtr[i] = rev
    }
    self.permutation = permPtr
    // No internal work buffer: `execute` re-points `workRe`/`workIm`
    // at the caller's output for the duration of the call.
  }

  deinit {
    for s in 0..<stageCount {
      twiddleRe[s].deallocate()
      twiddleIm[s].deallocate()
    }
    twiddleRe.deallocate()
    twiddleIm.deallocate()
    permutation.deallocate()
  }

  /// Run the N-point DFT. `inverse=false` is the unnormalised forward
  /// transform; `inverse=true` is the unnormalised inverse, so the caller
  /// is responsible for any `1/N` normalisation.
  func execute(
    realIn: UnsafePointer<Double>, imagIn: UnsafePointer<Double>,
    realOut: UnsafeMutablePointer<Double>, imagOut: UnsafeMutablePointer<Double>,
    inverse: Bool
  ) {
    // Aim the stage methods at the caller's output. The permute pass
    // writes there, every butterfly stage runs in-place on it, and the
    // result is already in the right place when we're done — no final
    // memcpy. (Allocating a separate work buffer + copying out doubles
    // the memory traffic for what's already a memory-bound kernel.)
    workRe = realOut
    workIm = imagOut

    // Step 1: permute input. For inverse, conjugate as we go
    // (DFT(x*) = (DFT(x))*, so we conjugate input then conjugate the
    // final output to flip the transform direction).
    if inverse {
      for i in 0..<n {
        let p = permutation[i]
        workRe[p] = realIn[i]
        workIm[p] = -imagIn[i]
      }
    } else {
      for i in 0..<n {
        let p = permutation[i]
        workRe[p] = realIn[i]
        workIm[p] = imagIn[i]
      }
    }

    // Step 2: butterfly stages, all in-place on (workRe, workIm) =
    // (realOut, imagOut).
    var m = 1
    for s in 0..<stageCount {
      let r = factors[s]
      let twRe = twiddleRe[s]
      let twIm = twiddleIm[s]
      switch r {
      case 2: stageRadix2(m: m, twRe: twRe, twIm: twIm)
      case 3: stageRadix3(m: m, twRe: twRe, twIm: twIm)
      case 4: stageRadix4(m: m, twRe: twRe, twIm: twIm)
      case 5: stageRadix5(m: m, twRe: twRe, twIm: twIm)
      case 7: stageRadix7(m: m, twRe: twRe, twIm: twIm)
      case 8: stageRadix8(m: m, twRe: twRe, twIm: twIm)
      default: fatalError("MixedRadixFFT: unsupported radix \(r)")
      }
      m *= r
    }

    // Step 3: re-conjugate the imaginary part for the inverse direction.
    // Forward direction is already done in place — no copy needed.
    if inverse {
      for i in 0..<n {
        imagOut[i] = -imagOut[i]
      }
    }
  }

  // MARK: - Stage implementations

  /// Apply radix-2 butterflies across `n / (m·2)` blocks of size `m·2`.
  /// Twiddle table layout: twRe[j·m + k] for j ∈ {0, 1}, k ∈ [0, m). The
  /// compiler will automatically vectorize this loop.
  @inline(__always)
  private func stageRadix2(
    m: Int, twRe: UnsafePointer<Double>, twIm: UnsafePointer<Double>
  ) {
    let blockSize = m * 2
    var b = 0
    while b < n {
      for k in 0..<m {
        let i0 = b &+ k
        let i1 = i0 &+ m
        let twR = twRe[m &+ k]
        let twI = twIm[m &+ k]
        let v1r = workRe[i1] * twR - workIm[i1] * twI
        let v1i = workRe[i1] * twI + workIm[i1] * twR
        let v0r = workRe[i0]
        let v0i = workIm[i0]
        workRe[i0] = v0r + v1r
        workIm[i0] = v0i + v1i
        workRe[i1] = v0r - v1r
        workIm[i1] = v0i - v1i
      }
      b = b &+ blockSize
    }
  }

  /// Apply radix-3 butterflies. Same layout as radix-2.
  @inline(__always)
  private func stageRadix3(
    m: Int, twRe: UnsafePointer<Double>, twIm: UnsafePointer<Double>
  ) {
    let blockSize = m * 3
    // W3 = exp(-2π i / 3) = (-1/2, -√3/2). The constant `√3/2` recurs below.
    let s32 = sin(2.0 * .pi / 3.0)  // √3/2 ≈ 0.86602540378
    var b = 0
    while b < n {
      for k in 0..<m {
        let i0 = b &+ k
        let i1 = i0 &+ m
        let i2 = i1 &+ m
        let tw1R = twRe[m &+ k]
        let tw1I = twIm[m &+ k]
        let tw2R = twRe[2 &* m &+ k]
        let tw2I = twIm[2 &* m &+ k]
        // Twiddle.
        let v1r = workRe[i1] * tw1R - workIm[i1] * tw1I
        let v1i = workRe[i1] * tw1I + workIm[i1] * tw1R
        let v2r = workRe[i2] * tw2R - workIm[i2] * tw2I
        let v2i = workRe[i2] * tw2I + workIm[i2] * tw2R
        let v0r = workRe[i0]
        let v0i = workIm[i0]
        // Radix-3 DFT.
        let sR = v1r + v2r
        let sI = v1i + v2i
        let dR = v1r - v2r
        let dI = v1i - v2i
        let aR = v0r - 0.5 * sR
        let aI = v0i - 0.5 * sI
        let bR = s32 * dR
        let bI = s32 * dI
        workRe[i0] = v0r + sR
        workIm[i0] = v0i + sI
        workRe[i1] = aR + bI
        workIm[i1] = aI - bR
        workRe[i2] = aR - bI
        workIm[i2] = aI + bR
      }
      b = b &+ blockSize
    }
  }

  /// Apply radix-4 butterflies. The inner DFT is multiplication-free —
  /// the four 4th-roots of unity are `{1, -i, -1, i}`, so the inner
  /// stage is just adds and ±i swaps. Only the 3 outer-stage twiddles
  /// (`v[1], v[2], v[3]`) cost real multiplies.
  @inline(__always)
  private func stageRadix4(
    m: Int, twRe: UnsafePointer<Double>, twIm: UnsafePointer<Double>
  ) {
    let blockSize = m * 4
    var b = 0
    while b < n {
      for k in 0..<m {
        let i0 = b &+ k
        let i1 = i0 &+ m
        let i2 = i1 &+ m
        let i3 = i2 &+ m
        let t1R = twRe[m &+ k]
        let t1I = twIm[m &+ k]
        let t2R = twRe[2 &* m &+ k]
        let t2I = twIm[2 &* m &+ k]
        let t3R = twRe[3 &* m &+ k]
        let t3I = twIm[3 &* m &+ k]
        let v0r = workRe[i0]
        let v0i = workIm[i0]
        let v1r = workRe[i1] * t1R - workIm[i1] * t1I
        let v1i = workRe[i1] * t1I + workIm[i1] * t1R
        let v2r = workRe[i2] * t2R - workIm[i2] * t2I
        let v2i = workRe[i2] * t2I + workIm[i2] * t2R
        let v3r = workRe[i3] * t3R - workIm[i3] * t3I
        let v3i = workRe[i3] * t3I + workIm[i3] * t3R
        let t0r = v0r + v2r
        let t0i = v0i + v2i
        let t1r2 = v0r - v2r
        let t1i2 = v0i - v2i
        let t2r2 = v1r + v3r
        let t2i2 = v1i + v3i
        let t3r2 = v1r - v3r
        let t3i2 = v1i - v3i
        workRe[i0] = t0r + t2r2
        workIm[i0] = t0i + t2i2
        workRe[i1] = t1r2 + t3i2
        workIm[i1] = t1i2 - t3r2
        workRe[i2] = t0r - t2r2
        workIm[i2] = t0i - t2i2
        workRe[i3] = t1r2 - t3i2
        workIm[i3] = t1i2 + t3r2
      }
      b = b &+ blockSize
    }
  }

  /// Apply radix-8 butterflies.
  @inline(__always)
  private func stageRadix8(
    m: Int, twRe: UnsafePointer<Double>, twIm: UnsafePointer<Double>
  ) {
    let blockSize = m * 8
    let s2 = 0.7071067811865476  // √2/2
    var b = 0
    while b < n {
      for k in 0..<m {
        let i0 = b &+ k
        let i1 = i0 &+ m
        let i2 = i1 &+ m
        let i3 = i2 &+ m
        let i4 = i3 &+ m
        let i5 = i4 &+ m
        let i6 = i5 &+ m
        let i7 = i6 &+ m
        let t1R = twRe[m &+ k]
        let t1I = twIm[m &+ k]
        let t2R = twRe[2 &* m &+ k]
        let t2I = twIm[2 &* m &+ k]
        let t3R = twRe[3 &* m &+ k]
        let t3I = twIm[3 &* m &+ k]
        let t4R = twRe[4 &* m &+ k]
        let t4I = twIm[4 &* m &+ k]
        let t5R = twRe[5 &* m &+ k]
        let t5I = twIm[5 &* m &+ k]
        let t6R = twRe[6 &* m &+ k]
        let t6I = twIm[6 &* m &+ k]
        let t7R = twRe[7 &* m &+ k]
        let t7I = twIm[7 &* m &+ k]
        let v0r = workRe[i0]
        let v0i = workIm[i0]
        let v1r = workRe[i1] * t1R - workIm[i1] * t1I
        let v1i = workRe[i1] * t1I + workIm[i1] * t1R
        let v2r = workRe[i2] * t2R - workIm[i2] * t2I
        let v2i = workRe[i2] * t2I + workIm[i2] * t2R
        let v3r = workRe[i3] * t3R - workIm[i3] * t3I
        let v3i = workRe[i3] * t3I + workIm[i3] * t3R
        let v4r = workRe[i4] * t4R - workIm[i4] * t4I
        let v4i = workRe[i4] * t4I + workIm[i4] * t4R
        let v5r = workRe[i5] * t5R - workIm[i5] * t5I
        let v5i = workRe[i5] * t5I + workIm[i5] * t5R
        let v6r = workRe[i6] * t6R - workIm[i6] * t6I
        let v6i = workRe[i6] * t6I + workIm[i6] * t6R
        let v7r = workRe[i7] * t7R - workIm[i7] * t7I
        let v7i = workRe[i7] * t7I + workIm[i7] * t7R
        let eA0r = v0r + v4r
        let eA0i = v0i + v4i
        let eA1r = v0r - v4r
        let eA1i = v0i - v4i
        let eA2r = v2r + v6r
        let eA2i = v2i + v6i
        let eA3r = v2r - v6r
        let eA3i = v2i - v6i
        let e0r = eA0r + eA2r
        let e0i = eA0i + eA2i
        let e1r = eA1r + eA3i
        let e1i = eA1i - eA3r
        let e2r = eA0r - eA2r
        let e2i = eA0i - eA2i
        let e3r = eA1r - eA3i
        let e3i = eA1i + eA3r
        let oA0r = v1r + v5r
        let oA0i = v1i + v5i
        let oA1r = v1r - v5r
        let oA1i = v1i - v5i
        let oA2r = v3r + v7r
        let oA2i = v3i + v7i
        let oA3r = v3r - v7r
        let oA3i = v3i - v7i
        let oo0r = oA0r + oA2r
        let oo0i = oA0i + oA2i
        let oo1r = oA1r + oA3i
        let oo1i = oA1i - oA3r
        let oo2r = oA0r - oA2r
        let oo2i = oA0i - oA2i
        let oo3r = oA1r - oA3i
        let oo3i = oA1i + oA3r
        let w0r = oo0r
        let w0i = oo0i
        let w1r = s2 * (oo1r + oo1i)
        let w1i = s2 * (oo1i - oo1r)
        let w2r = oo2i
        let w2i = -oo2r
        let w3r = s2 * (oo3i - oo3r)
        let w3i = -s2 * (oo3r + oo3i)
        workRe[i0] = e0r + w0r
        workIm[i0] = e0i + w0i
        workRe[i1] = e1r + w1r
        workIm[i1] = e1i + w1i
        workRe[i2] = e2r + w2r
        workIm[i2] = e2i + w2i
        workRe[i3] = e3r + w3r
        workIm[i3] = e3i + w3i
        workRe[i4] = e0r - w0r
        workIm[i4] = e0i - w0i
        workRe[i5] = e1r - w1r
        workIm[i5] = e1i - w1i
        workRe[i6] = e2r - w2r
        workIm[i6] = e2i - w2i
        workRe[i7] = e3r - w3r
        workIm[i7] = e3i - w3i
      }
      b = b &+ blockSize
    }
  }

  /// Apply radix-5 butterflies.
  @inline(__always)
  private func stageRadix5(
    m: Int, twRe: UnsafePointer<Double>, twIm: UnsafePointer<Double>
  ) {
    let blockSize = m * 5
    let w1R = MixedRadixFFT.c5_1Re
    let w1I = MixedRadixFFT.c5_1Im
    let w2R = MixedRadixFFT.c5_2Re
    let w2I = MixedRadixFFT.c5_2Im
    var b = 0
    while b < n {
      for k in 0..<m {
        let i0 = b &+ k
        let i1 = i0 &+ m
        let i2 = i1 &+ m
        let i3 = i2 &+ m
        let i4 = i3 &+ m
        let t1R = twRe[m &+ k]
        let t1I = twIm[m &+ k]
        let t2R = twRe[2 &* m &+ k]
        let t2I = twIm[2 &* m &+ k]
        let t3R = twRe[3 &* m &+ k]
        let t3I = twIm[3 &* m &+ k]
        let t4R = twRe[4 &* m &+ k]
        let t4I = twIm[4 &* m &+ k]
        let v1r = workRe[i1] * t1R - workIm[i1] * t1I
        let v1i = workRe[i1] * t1I + workIm[i1] * t1R
        let v2r = workRe[i2] * t2R - workIm[i2] * t2I
        let v2i = workRe[i2] * t2I + workIm[i2] * t2R
        let v3r = workRe[i3] * t3R - workIm[i3] * t3I
        let v3i = workRe[i3] * t3I + workIm[i3] * t3R
        let v4r = workRe[i4] * t4R - workIm[i4] * t4I
        let v4i = workRe[i4] * t4I + workIm[i4] * t4R
        let v0r = workRe[i0]
        let v0i = workIm[i0]
        let sum14R = v1r + v4r
        let sum14I = v1i + v4i
        let diff14R = v1r - v4r
        let diff14I = v1i - v4i
        let sum23R = v2r + v3r
        let sum23I = v2i + v3i
        let diff23R = v2r - v3r
        let diff23I = v2i - v3i
        workRe[i0] = v0r + sum14R + sum23R
        workIm[i0] = v0i + sum14I + sum23I
        let cR14 = w1R * sum14R + w2R * sum23R
        let cI14 = w1R * sum14I + w2R * sum23I
        let tR14 = w1I * diff14I + w2I * diff23I
        let tI14 = w1I * diff14R + w2I * diff23R
        workRe[i1] = v0r + cR14 - tR14
        workIm[i1] = v0i + cI14 + tI14
        workRe[i4] = v0r + cR14 + tR14
        workIm[i4] = v0i + cI14 - tI14
        let cR23 = w2R * sum14R + w1R * sum23R
        let cI23 = w2R * sum14I + w1R * sum23I
        let tR23 = w2I * diff14I - w1I * diff23I
        let tI23 = w2I * diff14R - w1I * diff23R
        workRe[i2] = v0r + cR23 - tR23
        workIm[i2] = v0i + cI23 + tI23
        workRe[i3] = v0r + cR23 + tR23
        workIm[i3] = v0i + cI23 - tI23
      }
      b = b &+ blockSize
    }
  }

  /// Apply radix-7 butterflies.
  @inline(__always)
  private func stageRadix7(
    m: Int, twRe: UnsafePointer<Double>, twIm: UnsafePointer<Double>
  ) {
    let blockSize = m * 7
    let w1R = MixedRadixFFT.c7_1Re
    let w1I = MixedRadixFFT.c7_1Im
    let w2R = MixedRadixFFT.c7_2Re
    let w2I = MixedRadixFFT.c7_2Im
    let w3R = MixedRadixFFT.c7_3Re
    let w3I = MixedRadixFFT.c7_3Im
    var b = 0
    while b < n {
      for k in 0..<m {
        let i0 = b &+ k
        let i1 = i0 &+ m
        let i2 = i1 &+ m
        let i3 = i2 &+ m
        let i4 = i3 &+ m
        let i5 = i4 &+ m
        let i6 = i5 &+ m
        let t1R = twRe[m &+ k]
        let t1I = twIm[m &+ k]
        let t2R = twRe[2 &* m &+ k]
        let t2I = twIm[2 &* m &+ k]
        let t3R = twRe[3 &* m &+ k]
        let t3I = twIm[3 &* m &+ k]
        let t4R = twRe[4 &* m &+ k]
        let t4I = twIm[4 &* m &+ k]
        let t5R = twRe[5 &* m &+ k]
        let t5I = twIm[5 &* m &+ k]
        let t6R = twRe[6 &* m &+ k]
        let t6I = twIm[6 &* m &+ k]
        let v1r = workRe[i1] * t1R - workIm[i1] * t1I
        let v1i = workRe[i1] * t1I + workIm[i1] * t1R
        let v2r = workRe[i2] * t2R - workIm[i2] * t2I
        let v2i = workRe[i2] * t2I + workIm[i2] * t2R
        let v3r = workRe[i3] * t3R - workIm[i3] * t3I
        let v3i = workRe[i3] * t3I + workIm[i3] * t3R
        let v4r = workRe[i4] * t4R - workIm[i4] * t4I
        let v4i = workRe[i4] * t4I + workIm[i4] * t4R
        let v5r = workRe[i5] * t5R - workIm[i5] * t5I
        let v5i = workRe[i5] * t5I + workIm[i5] * t5R
        let v6r = workRe[i6] * t6R - workIm[i6] * t6I
        let v6i = workRe[i6] * t6I + workIm[i6] * t6R
        let v0r = workRe[i0]
        let v0i = workIm[i0]
        let s16R = v1r + v6r
        let s16I = v1i + v6i
        let d16R = v1r - v6r
        let d16I = v1i - v6i
        let s25R = v2r + v5r
        let s25I = v2i + v5i
        let d25R = v2r - v5r
        let d25I = v2i - v5i
        let s34R = v3r + v4r
        let s34I = v3i + v4i
        let d34R = v3r - v4r
        let d34I = v3i - v4i
        workRe[i0] = v0r + s16R + s25R + s34R
        workIm[i0] = v0i + s16I + s25I + s34I
        let cR16 = w1R * s16R + w2R * s25R + w3R * s34R
        let cI16 = w1R * s16I + w2R * s25I + w3R * s34I
        let tR16 = w1I * d16I + w2I * d25I + w3I * d34I
        let tI16 = w1I * d16R + w2I * d25R + w3I * d34R
        workRe[i1] = v0r + cR16 - tR16
        workIm[i1] = v0i + cI16 + tI16
        workRe[i6] = v0r + cR16 + tR16
        workIm[i6] = v0i + cI16 - tI16
        let cR25 = w2R * s16R + w3R * s25R + w1R * s34R
        let cI25 = w2R * s16I + w3R * s25I + w1R * s34I
        let tR25 = w2I * d16I - w3I * d25I - w1I * d34I
        let tI25 = w2I * d16R - w3I * d25R - w1I * d34R
        workRe[i2] = v0r + cR25 - tR25
        workIm[i2] = v0i + cI25 + tI25
        workRe[i5] = v0r + cR25 + tR25
        workIm[i5] = v0i + cI25 - tI25
        let cR34 = w3R * s16R + w1R * s25R + w2R * s34R
        let cI34 = w3R * s16I + w1R * s25I + w2R * s34I
        let tR34 = w3I * d16I - w1I * d25I + w2I * d34I
        let tI34 = w3I * d16R - w1I * d25R + w2I * d34R
        workRe[i3] = v0r + cR34 - tR34
        workIm[i3] = v0i + cI34 + tI34
        workRe[i4] = v0r + cR34 + tR34
        workIm[i4] = v0i + cI34 - tI34
      }
      b = b &+ blockSize
    }
  }
}
