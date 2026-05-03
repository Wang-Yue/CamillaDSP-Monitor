// Native mixed-radix Cooley-Tukey FFT for arbitrary lengths whose prime
// factors are all ≤ 7. Targets `N = 1029 = 3 · 7³` and `N = 1120 = 2⁵ · 5 · 7`
// — the inner FFT sizes that BluesteinRealFFT needs for 44.1↔48 kHz
// resampling. Compared with Bluestein-on-vDSP, this trades the inner
// power-of-2 transforms (M = 4096) for a direct decomposition into
// `O(N · Σ pᵢ)` ops — about 6× fewer arithmetic operations at N = 1029.
//
// Architecture: classic iterative DIT (decimation-in-time) Cooley-Tukey.
//   1. Permute input via mixed-radix digit reversal.
//   2. For each factor `r` (in order), apply length-`r` butterflies on
//      stride-`m` groups, where `m` grows by `r` after each stage. Twiddle
//      factors W_{m·r}^(j·k) are pre-computed once at init.
//   3. Copy out (with conjugation for the inverse direction).
//
// Inverse FFT uses the identity `IDFT(x) = conj(DFT(conj(x)))`, so we only
// pre-compute the forward twiddles. Both transforms are unnormalised —
// matches the `realfft` convention.
//
// All buffers (twiddles, permutation LUT, scratch) are heap-allocated at
// init and freed in deinit. The hot path runs purely on raw pointers — no
// allocations, no closures.

import Foundation

/// Mixed-radix complex FFT supporting `N = 2^a · 3^b · 5^c · 7^d`. Returns
/// `nil` if `N` has any prime factor > 7 — caller should fall back to
/// Bluestein in that case.
final class MixedRadixFFT {
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

  /// Constants used by the radix-3, 5, 7 butterflies. Pre-computed for the
  /// forward direction; the inverse path conjugates the input/output, so
  /// the same constants work for both.
  private static let c3Re: Double = -0.5  // cos(2π/3)
  private static let c3Im: Double = -sin(2.0 * .pi / 3.0)  // -sin(2π/3) = -√3/2

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
  /// transform; `inverse=true` is the unnormalised inverse — both match
  /// `realfft`'s convention, so the caller is responsible for any
  /// `1/N` normalisation.
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
  /// SIMD2 path processes pairs of `k` for `m ≥ 2`; the scalar tail handles
  /// odd `m` (and the m=1 stages where the k loop has just one iteration).
  @inline(__always)
  private func stageRadix2(
    m: Int, twRe: UnsafePointer<Double>, twIm: UnsafePointer<Double>
  ) {
    let blockSize = m * 2
    let mPairs = m & ~1
    var b = 0
    while b < n {
      var k = 0
      while k < mPairs {
        let i0 = b + k
        let i1 = i0 + m
        let mk = m + k
        let twR = ldSIMD2(twRe, mk)
        let twI = ldSIMD2(twIm, mk)
        let v0r = ldSIMD2(workRe, i0)
        let v0i = ldSIMD2(workIm, i0)
        let v1rRaw = ldSIMD2(workRe, i1)
        let v1iRaw = ldSIMD2(workIm, i1)
        let v1r = v1rRaw * twR - v1iRaw * twI
        let v1i = v1rRaw * twI + v1iRaw * twR
        stSIMD2(workRe, i0, v0r + v1r)
        stSIMD2(workIm, i0, v0i + v1i)
        stSIMD2(workRe, i1, v0r - v1r)
        stSIMD2(workIm, i1, v0i - v1i)
        k += 2
      }
      while k < m {
        let i0 = b + k
        let i1 = i0 + m
        let twR = twRe[m + k]
        let twI = twIm[m + k]
        let v1r = workRe[i1] * twR - workIm[i1] * twI
        let v1i = workRe[i1] * twI + workIm[i1] * twR
        let v0r = workRe[i0]
        let v0i = workIm[i0]
        workRe[i0] = v0r + v1r
        workIm[i0] = v0i + v1i
        workRe[i1] = v0r - v1r
        workIm[i1] = v0i - v1i
        k += 1
      }
      b += blockSize
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
    let mPairs = m & ~1
    let m2 = m << 1
    var b = 0
    while b < n {
      var k = 0
      while k < mPairs {
        let i0 = b + k
        let i1 = i0 + m
        let i2 = i1 + m
        let mk = m + k
        let m2k = m2 + k

        let t1R = ldSIMD2(twRe, mk)
        let t1I = ldSIMD2(twIm, mk)
        let t2R = ldSIMD2(twRe, m2k)
        let t2I = ldSIMD2(twIm, m2k)

        let r0r = ldSIMD2(workRe, i0)
        let r0i = ldSIMD2(workIm, i0)
        let r1rRaw = ldSIMD2(workRe, i1)
        let r1iRaw = ldSIMD2(workIm, i1)
        let r2rRaw = ldSIMD2(workRe, i2)
        let r2iRaw = ldSIMD2(workIm, i2)

        let v1r = r1rRaw * t1R - r1iRaw * t1I
        let v1i = r1rRaw * t1I + r1iRaw * t1R
        let v2r = r2rRaw * t2R - r2iRaw * t2I
        let v2i = r2rRaw * t2I + r2iRaw * t2R

        let sR = v1r + v2r
        let sI = v1i + v2i
        let dR = v1r - v2r
        let dI = v1i - v2i
        let aR = r0r - 0.5 * sR
        let aI = r0i - 0.5 * sI
        let bR = s32 * dR
        let bI = s32 * dI

        stSIMD2(workRe, i0, r0r + sR)
        stSIMD2(workIm, i0, r0i + sI)
        stSIMD2(workRe, i1, aR + bI)
        stSIMD2(workIm, i1, aI - bR)
        stSIMD2(workRe, i2, aR - bI)
        stSIMD2(workIm, i2, aI + bR)
        k += 2
      }
      while k < m {
        let i0 = b + k
        let i1 = i0 + m
        let i2 = i1 + m
        let tw1R = twRe[m + k]
        let tw1I = twIm[m + k]
        let tw2R = twRe[2 * m + k]
        let tw2I = twIm[2 * m + k]
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
        k += 1
      }
      b += blockSize
    }
  }

  /// Apply radix-4 butterflies. The inner DFT is multiplication-free —
  /// the four 4th-roots of unity are `{1, -i, -1, i}`, so the inner
  /// stage is just adds and ±i swaps. Only the 3 outer-stage twiddles
  /// (`v[1], v[2], v[3]`) cost real multiplies.
  ///
  /// SIMD2 path over consecutive `k` pairs; scalar tail for odd `m`.
  @inline(__always)
  private func stageRadix4(
    m: Int, twRe: UnsafePointer<Double>, twIm: UnsafePointer<Double>
  ) {
    let blockSize = m * 4
    let mPairs = m & ~1
    var b = 0
    while b < n {
      var k = 0
      while k < mPairs {
        let i0 = b + k
        let i1 = i0 + m
        let i2 = i1 + m
        let i3 = i2 + m
        let mk = m + k
        let m2k = (m << 1) + k
        let m3k = (m + (m << 1)) + k
        let t1R = ldSIMD2(twRe, mk)
        let t1I = ldSIMD2(twIm, mk)
        let t2R = ldSIMD2(twRe, m2k)
        let t2I = ldSIMD2(twIm, m2k)
        let t3R = ldSIMD2(twRe, m3k)
        let t3I = ldSIMD2(twIm, m3k)
        let v0r = ldSIMD2(workRe, i0)
        let v0i = ldSIMD2(workIm, i0)
        let r1r = ldSIMD2(workRe, i1)
        let r1i = ldSIMD2(workIm, i1)
        let r2r = ldSIMD2(workRe, i2)
        let r2i = ldSIMD2(workIm, i2)
        let r3r = ldSIMD2(workRe, i3)
        let r3i = ldSIMD2(workIm, i3)
        let v1r = r1r * t1R - r1i * t1I
        let v1i = r1r * t1I + r1i * t1R
        let v2r = r2r * t2R - r2i * t2I
        let v2i = r2r * t2I + r2i * t2R
        let v3r = r3r * t3R - r3i * t3I
        let v3i = r3r * t3I + r3i * t3R
        // Inner radix-4 DFT: T0=v0+v2, T1=v0-v2, T2=v1+v3, T3=v1-v3
        // O[0]=T0+T2, O[1]=T1-i·T3, O[2]=T0-T2, O[3]=T1+i·T3
        // -i·z = (z.im, -z.re); +i·z = (-z.im, z.re).
        let t0r = v0r + v2r
        let t0i = v0i + v2i
        let t1r = v0r - v2r
        let t1i = v0i - v2i
        let t2r = v1r + v3r
        let t2i = v1i + v3i
        let t3r = v1r - v3r
        let t3i = v1i - v3i
        let o0r = t0r + t2r
        let o0i = t0i + t2i
        let o1r = t1r + t3i
        let o1i = t1i - t3r
        let o2r = t0r - t2r
        let o2i = t0i - t2i
        let o3r = t1r - t3i
        let o3i = t1i + t3r
        stSIMD2(workRe, i0, o0r)
        stSIMD2(workIm, i0, o0i)
        stSIMD2(workRe, i1, o1r)
        stSIMD2(workIm, i1, o1i)
        stSIMD2(workRe, i2, o2r)
        stSIMD2(workIm, i2, o2i)
        stSIMD2(workRe, i3, o3r)
        stSIMD2(workIm, i3, o3i)
        k += 2
      }
      while k < m {
        let i0 = b + k
        let i1 = i0 + m
        let i2 = i1 + m
        let i3 = i2 + m
        let t1R = twRe[m + k]
        let t1I = twIm[m + k]
        let t2R = twRe[2 * m + k]
        let t2I = twIm[2 * m + k]
        let t3R = twRe[3 * m + k]
        let t3I = twIm[3 * m + k]
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
        k += 1
      }
      b += blockSize
    }
  }

  /// Apply radix-8 butterflies. The inner DFT is computed via DIT
  /// decomposition into two radix-4s (even-indexed and odd-indexed),
  /// then combined with the trivial 8th-root twiddles
  /// `W_8^k = exp(-2πi·k/8)`. Multiplications cost only the constant
  /// `√2/2` for the k=1 and k=3 inner twiddles — k=0 is free, k=2 is
  /// `-i` (free), so no real-coefficient multiplies on the inner DFT
  /// beyond the two `√2/2` cross-terms.
  @inline(__always)
  private func stageRadix8(
    m: Int, twRe: UnsafePointer<Double>, twIm: UnsafePointer<Double>
  ) {
    let blockSize = m * 8
    let mPairs = m & ~1
    let s2 = 0.7071067811865476  // √2/2
    var b = 0
    while b < n {
      var k = 0
      while k < mPairs {
        let i0 = b + k
        let i1 = i0 + m
        let i2 = i1 + m
        let i3 = i2 + m
        let i4 = i3 + m
        let i5 = i4 + m
        let i6 = i5 + m
        let i7 = i6 + m
        let mk = m + k
        let m2k = (m << 1) + k
        let m3k = m2k + m  // 3m + k
        let m4k = (m << 2) + k  // 4m + k
        let m5k = m4k + m  // 5m + k
        let m6k = m4k + (m << 1)  // 6m + k
        let m7k = m6k + m  // 7m + k
        let t1R = ldSIMD2(twRe, mk)
        let t1I = ldSIMD2(twIm, mk)
        let t2R = ldSIMD2(twRe, m2k)
        let t2I = ldSIMD2(twIm, m2k)
        let t3R = ldSIMD2(twRe, m3k)
        let t3I = ldSIMD2(twIm, m3k)
        let t4R = ldSIMD2(twRe, m4k)
        let t4I = ldSIMD2(twIm, m4k)
        let t5R = ldSIMD2(twRe, m5k)
        let t5I = ldSIMD2(twIm, m5k)
        let t6R = ldSIMD2(twRe, m6k)
        let t6I = ldSIMD2(twIm, m6k)
        let t7R = ldSIMD2(twRe, m7k)
        let t7I = ldSIMD2(twIm, m7k)
        let v0r = ldSIMD2(workRe, i0)
        let v0i = ldSIMD2(workIm, i0)
        let r1r = ldSIMD2(workRe, i1)
        let r1i = ldSIMD2(workIm, i1)
        let r2r = ldSIMD2(workRe, i2)
        let r2i = ldSIMD2(workIm, i2)
        let r3r = ldSIMD2(workRe, i3)
        let r3i = ldSIMD2(workIm, i3)
        let r4r = ldSIMD2(workRe, i4)
        let r4i = ldSIMD2(workIm, i4)
        let r5r = ldSIMD2(workRe, i5)
        let r5i = ldSIMD2(workIm, i5)
        let r6r = ldSIMD2(workRe, i6)
        let r6i = ldSIMD2(workIm, i6)
        let r7r = ldSIMD2(workRe, i7)
        let r7i = ldSIMD2(workIm, i7)
        let v1r = r1r * t1R - r1i * t1I
        let v1i = r1r * t1I + r1i * t1R
        let v2r = r2r * t2R - r2i * t2I
        let v2i = r2r * t2I + r2i * t2R
        let v3r = r3r * t3R - r3i * t3I
        let v3i = r3r * t3I + r3i * t3R
        let v4r = r4r * t4R - r4i * t4I
        let v4i = r4r * t4I + r4i * t4R
        let v5r = r5r * t5R - r5i * t5I
        let v5i = r5r * t5I + r5i * t5R
        let v6r = r6r * t6R - r6i * t6I
        let v6i = r6r * t6I + r6i * t6R
        let v7r = r7r * t7R - r7i * t7I
        let v7i = r7r * t7I + r7i * t7R
        // Even radix-4: DFT of (v0, v2, v4, v6).
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
        // Odd radix-4: DFT of (v1, v3, v5, v7).
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
        // Apply W_8^k to odd outputs:
        //   W_8^0 = 1; W_8^1 = (s2, -s2); W_8^2 = -i; W_8^3 = (-s2, -s2).
        let w0r = oo0r
        let w0i = oo0i
        let w1r = s2 * (oo1r + oo1i)
        let w1i = s2 * (oo1i - oo1r)
        let w2r = oo2i
        let w2i = -oo2r
        let w3r = s2 * (oo3i - oo3r)
        let w3i = -s2 * (oo3r + oo3i)
        // O[k] = E[k] + W_8^k·O_odd[k], O[k+4] = E[k] - W_8^k·O_odd[k].
        let o0r = e0r + w0r
        let o0i = e0i + w0i
        let o1r = e1r + w1r
        let o1i = e1i + w1i
        let o2r = e2r + w2r
        let o2i = e2i + w2i
        let o3r = e3r + w3r
        let o3i = e3i + w3i
        let o4r = e0r - w0r
        let o4i = e0i - w0i
        let o5r = e1r - w1r
        let o5i = e1i - w1i
        let o6r = e2r - w2r
        let o6i = e2i - w2i
        let o7r = e3r - w3r
        let o7i = e3i - w3i
        stSIMD2(workRe, i0, o0r)
        stSIMD2(workIm, i0, o0i)
        stSIMD2(workRe, i1, o1r)
        stSIMD2(workIm, i1, o1i)
        stSIMD2(workRe, i2, o2r)
        stSIMD2(workIm, i2, o2i)
        stSIMD2(workRe, i3, o3r)
        stSIMD2(workIm, i3, o3i)
        stSIMD2(workRe, i4, o4r)
        stSIMD2(workIm, i4, o4i)
        stSIMD2(workRe, i5, o5r)
        stSIMD2(workIm, i5, o5i)
        stSIMD2(workRe, i6, o6r)
        stSIMD2(workIm, i6, o6i)
        stSIMD2(workRe, i7, o7r)
        stSIMD2(workIm, i7, o7i)
        k += 2
      }
      while k < m {
        let i0 = b + k
        let i1 = i0 + m
        let i2 = i1 + m
        let i3 = i2 + m
        let i4 = i3 + m
        let i5 = i4 + m
        let i6 = i5 + m
        let i7 = i6 + m
        let t1R = twRe[m + k]
        let t1I = twIm[m + k]
        let t2R = twRe[2 * m + k]
        let t2I = twIm[2 * m + k]
        let t3R = twRe[3 * m + k]
        let t3I = twIm[3 * m + k]
        let t4R = twRe[4 * m + k]
        let t4I = twIm[4 * m + k]
        let t5R = twRe[5 * m + k]
        let t5I = twIm[5 * m + k]
        let t6R = twRe[6 * m + k]
        let t6I = twIm[6 * m + k]
        let t7R = twRe[7 * m + k]
        let t7I = twIm[7 * m + k]
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
        k += 1
      }
      b += blockSize
    }
  }

  /// Apply radix-5 butterflies.
  @inline(__always)
  private func stageRadix5(
    m: Int, twRe: UnsafePointer<Double>, twIm: UnsafePointer<Double>
  ) {
    let blockSize = m * 5
    // Radix-5 uses these inner DFT constants. tw_5^k = exp(-2πi·k/5).
    let w1R = MixedRadixFFT.c5_1Re
    let w1I = MixedRadixFFT.c5_1Im
    let w2R = MixedRadixFFT.c5_2Re
    let w2I = MixedRadixFFT.c5_2Im
    let mPairs = m & ~1
    let m2 = m << 1
    let m3 = m2 + m
    let m4 = m << 2
    var b = 0
    while b < n {
      var k = 0
      while k < mPairs {
        let i0 = b + k
        let i1 = i0 + m
        let i2 = i1 + m
        let i3 = i2 + m
        let i4 = i3 + m

        let mk = m + k
        let m2k = m2 + k
        let m3k = m3 + k
        let m4k = m4 + k

        let t1R = ldSIMD2(twRe, mk)
        let t1I = ldSIMD2(twIm, mk)
        let t2R = ldSIMD2(twRe, m2k)
        let t2I = ldSIMD2(twIm, m2k)
        let t3R = ldSIMD2(twRe, m3k)
        let t3I = ldSIMD2(twIm, m3k)
        let t4R = ldSIMD2(twRe, m4k)
        let t4I = ldSIMD2(twIm, m4k)

        let r0r = ldSIMD2(workRe, i0)
        let r0i = ldSIMD2(workIm, i0)
        let r1rRaw = ldSIMD2(workRe, i1)
        let r1iRaw = ldSIMD2(workIm, i1)
        let r2rRaw = ldSIMD2(workRe, i2)
        let r2iRaw = ldSIMD2(workIm, i2)
        let r3rRaw = ldSIMD2(workRe, i3)
        let r3iRaw = ldSIMD2(workIm, i3)
        let r4rRaw = ldSIMD2(workRe, i4)
        let r4iRaw = ldSIMD2(workIm, i4)

        let v1r = r1rRaw * t1R - r1iRaw * t1I
        let v1i = r1rRaw * t1I + r1iRaw * t1R
        let v2r = r2rRaw * t2R - r2iRaw * t2I
        let v2i = r2rRaw * t2I + r2iRaw * t2R
        let v3r = r3rRaw * t3R - r3iRaw * t3I
        let v3i = r3rRaw * t3I + r3iRaw * t3R
        let v4r = r4rRaw * t4R - r4iRaw * t4I
        let v4i = r4rRaw * t4I + r4iRaw * t4R

        let sum14R = v1r + v4r
        let sum14I = v1i + v4i
        let diff14R = v1r - v4r
        let diff14I = v1i - v4i
        let sum23R = v2r + v3r
        let sum23I = v2i + v3i
        let diff23R = v2r - v3r
        let diff23I = v2i - v3i

        stSIMD2(workRe, i0, r0r + sum14R + sum23R)
        stSIMD2(workIm, i0, r0i + sum14I + sum23I)

        stSIMD2(workRe, i1, r0r + w1R * sum14R - w1I * diff14I + w2R * sum23R - w2I * diff23I)
        stSIMD2(workIm, i1, r0i + w1R * sum14I + w1I * diff14R + w2R * sum23I + w2I * diff23R)

        stSIMD2(workRe, i2, r0r + w2R * sum14R - w2I * diff14I + w1R * sum23R - (-w1I) * diff23I)
        stSIMD2(workIm, i2, r0i + w2R * sum14I + w2I * diff14R + w1R * sum23I + (-w1I) * diff23R)

        stSIMD2(workRe, i3, r0r + w2R * sum14R - (-w2I) * diff14I + w1R * sum23R - w1I * diff23I)
        stSIMD2(workIm, i3, r0i + w2R * sum14I + (-w2I) * diff14R + w1R * sum23I + w1I * diff23R)

        stSIMD2(workRe, i4, r0r + w1R * sum14R - (-w1I) * diff14I + w2R * sum23R - (-w2I) * diff23I)
        stSIMD2(workIm, i4, r0i + w1R * sum14I + (-w1I) * diff14R + w2R * sum23I + (-w2I) * diff23R)

        k += 2
      }
      while k < m {
        let i0 = b + k
        let i1 = i0 + m
        let i2 = i1 + m
        let i3 = i2 + m
        let i4 = i3 + m
        // Outer-stage twiddle on samples 1..4.
        let t1R = twRe[m + k]
        let t1I = twIm[m + k]
        let t2R = twRe[2 * m + k]
        let t2I = twIm[2 * m + k]
        let t3R = twRe[3 * m + k]
        let t3I = twIm[3 * m + k]
        let t4R = twRe[4 * m + k]
        let t4I = twIm[4 * m + k]
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
        // Radix-5 DFT (direct, not Winograd). 4 unique inner products plus
        // the DC term — straightforward and lets the compiler issue plenty
        // of FMAs.
        // O[0] = v0 + v1 + v2 + v3 + v4
        // O[k] = v0 + W^k·v1 + W^(2k)·v2 + W^(3k)·v3 + W^(4k)·v4
        // where W = exp(-2πi/5). W^4 = conj(W), W^3 = conj(W²).
        // So we need {1, W, W², W³ = conj(W²), W⁴ = conj(W)}.
        let sum14R = v1r + v4r
        let sum14I = v1i + v4i
        let diff14R = v1r - v4r
        let diff14I = v1i - v4i
        let sum23R = v2r + v3r
        let sum23I = v2i + v3i
        let diff23R = v2r - v3r
        let diff23I = v2i - v3i
        // O[0]
        workRe[i0] = v0r + sum14R + sum23R
        workIm[i0] = v0i + sum14I + sum23I
        // For O[1]: v0 + W·v1 + W²·v2 + W³·v3 + W⁴·v4
        // = v0 + (W·v1 + conj(W)·v4) + (W²·v2 + conj(W²)·v3)
        // For each pair (z·v1 + conj(z)·v4):
        //   = (zR+I·zI)*(v1R+I·v1I) + (zR-I·zI)*(v4R+I·v4I)
        //   = zR·(v1R+v4R) - zI·(v1I-v4I)  + I·(zR·(v1I+v4I) + zI·(v1R-v4R))
        //   = zR·sum14R - zI·diff14I       + I·(zR·sum14I + zI·diff14R)
        // O[1] = v0 + (w1R·sum14R - w1I·diff14I) + (w2R·sum23R - w2I·diff23I)
        //          + I·[ (w1R·sum14I + w1I·diff14R) + (w2R·sum23I + w2I·diff23R) ]
        workRe[i1] =
          v0r + w1R * sum14R - w1I * diff14I + w2R * sum23R - w2I * diff23I
        workIm[i1] =
          v0i + w1R * sum14I + w1I * diff14R + w2R * sum23I + w2I * diff23R
        // O[2] uses W² and W⁴ for the (1,4) pair, W and W³ for the (2,3) pair.
        // Effective (zR, zI) for sum14 is (w2R, w2I); for sum23 is (w1R, w1I)
        //   — wait, k=2 means we want W²·v1 + W⁴·v4 for the 14-pair, which
        // is (w2R, w2I)·v1 + conj((w2R, w2I))·v4 → coefficients (w2R, w2I).
        // For k=2, the 23-pair: W^(2·2)=W^4=conj(W), W^(2·3)=W^6=W·W^5=W·1=W.
        // So pair is W^4·v2 + W·v3 with coefficients (conj(W), W) →
        // (w1R, -w1I) for v2, (w1R, w1I) for v3. After symmetrising:
        // result = (w1R·sum23 - (-w1I)·diff23·...) — let me redo this generically.
        //
        // General formula for O[k]: v0 + sum_{p=1..2} [(coef_p·sum_{2p-1,5-(2p-1)}-diff terms]
        //   coefficients depend on k.
        // For radix-5, the standard split-radix coefficients:
        //   O[1] uses (w1, w2) for (sum14, sum23)
        //   O[2] uses (w2, w1*) where w1* means conj on the imag (effectively w_3 = conj(w_2))
        //   O[3] uses (w2*, w1)  (conjugate of O[2])
        //   O[4] uses (w1*, w2*) (conjugate of O[1])
        // Easier: explicitly compute O[3] = conj(O[2]) and O[4] = conj(O[1]) using real input.
        // BUT input is complex here, so we can't use that shortcut directly.
        //
        // Direct formula for O[2]:
        //   coefs for (sum14, diff14) are (w2R, w2I) (because W^(2·1) = W²)
        //   coefs for (sum23, diff23) are (w_{2·2 mod 5}, ...) = W^4 = conj(W) → (w1R, -w1I)
        // Hmm, but we paired (2, 3) with (W², W³) where W³ = conj(W²) = (w2R, -w2I).
        // So the 23-pair for O[2] uses coefficient (W^(2·2), W^(2·3)) = (W^4, W^6) = (W^-1, W^1) = (conj(W), W) = ((w1R, -w1I), (w1R, w1I)).
        // For (a·v2 + b·v3) with a = (w1R, -w1I), b = (w1R, w1I):
        //   re = w1R·(v2r+v3r) - (-w1I)·v2i - w1I·v3i
        //      = w1R·sum23R + w1I·(v2i - v3i)
        //      = w1R·sum23R + w1I·(-diff23I)
        //      = w1R·sum23R - w1I·diff23I  [hmm wait that's the same as (w2R, w2I)? no]
        // Hmm I keep confusing myself. Let me restart with a clear formula.
        //
        // O[k] = v0 + sum_{j=1..4} W^(j·k) · v_j
        //      = v0 + W^k·v1 + W^(2k)·v2 + W^(3k)·v3 + W^(4k)·v4
        // For real W^(j·k) values, define eR_jk, eI_jk = Re/Im of W^(j·k).
        // Pair {j=1, j=4} = {W^k, W^(4k)} = {W^k, W^(-k)} = {W^k, conj(W^k)}.
        // Pair {j=2, j=3} = {W^(2k), W^(3k)} = {W^(2k), W^(-2k)} = {W^(2k), conj(W^(2k))}.
        // So for each pair, coefficients are (z, conj(z)) where z = W^(j·k) for the smaller j.
        // (z·v + conj(z)·v') where v=v1, v'=v4 (for pair 1):
        //   re = zR·(v1r+v4r) - zI·(v1i-v4i) = zR·sum14R - zI·diff14I
        //   im = zR·(v1i+v4i) + zI·(v1r-v4r) = zR·sum14I + zI·diff14R
        // So with `paircoef(z) = (zR, zI)`:
        //   pairResult.re = zR·sumR - zI·diffI
        //   pairResult.im = zR·sumI + zI·diffR
        //
        // For O[1]: z14 = W^1 = (w1R, w1I), z23 = W^2 = (w2R, w2I)
        // For O[2]: z14 = W^2 = (w2R, w2I), z23 = W^4 = conj(W) = (w1R, -w1I)
        // For O[3]: z14 = W^3 = conj(W^2) = (w2R, -w2I), z23 = W^6 = W = (w1R, w1I)
        // For O[4]: z14 = W^4 = conj(W) = (w1R, -w1I), z23 = W^8 = W^3 = (w2R, -w2I)
        //
        // (Already computed O[1] above.)
        // O[2]:
        workRe[i2] =
          v0r + w2R * sum14R - w2I * diff14I + w1R * sum23R - (-w1I) * diff23I
        workIm[i2] =
          v0i + w2R * sum14I + w2I * diff14R + w1R * sum23I + (-w1I) * diff23R
        // O[3]:
        workRe[i3] =
          v0r + w2R * sum14R - (-w2I) * diff14I + w1R * sum23R - w1I * diff23I
        workIm[i3] =
          v0i + w2R * sum14I + (-w2I) * diff14R + w1R * sum23I + w1I * diff23R
        // O[4]:
        workRe[i4] =
          v0r + w1R * sum14R - (-w1I) * diff14I + w2R * sum23R - (-w2I) * diff23I
        workIm[i4] =
          v0i + w1R * sum14I + (-w1I) * diff14R + w2R * sum23I + (-w2I) * diff23R
        k += 1
      }
      b += blockSize
    }
  }

  /// Apply radix-7 butterflies. Direct DFT — 6 unique pairs of conjugate
  /// twiddles. Compute each output as `v0 + Σ pair-products`.
  ///
  /// Two paths: for `m ≥ 2`, the inner `k` loop is unrolled by 2 with NEON
  /// `SIMD2<Double>` so each butterfly's 6 twiddle multiplies + 6 sum/diff
  /// pairs + 7 outputs run on 2 lanes simultaneously. The scalar tail handles
  /// the odd `k` (and the entire stage when `m == 1`, where the inner loop
  /// has only one iteration anyway).
  @inline(__always)
  private func stageRadix7(
    m: Int, twRe: UnsafePointer<Double>, twIm: UnsafePointer<Double>
  ) {
    let blockSize = m * 7
    let mPairs = m & ~1  // largest even ≤ m
    let w1R = MixedRadixFFT.c7_1Re
    let w1I = MixedRadixFFT.c7_1Im
    let w2R = MixedRadixFFT.c7_2Re
    let w2I = MixedRadixFFT.c7_2Im
    let w3R = MixedRadixFFT.c7_3Re
    let w3I = MixedRadixFFT.c7_3Im
    // Cache the m-multiples once per stage call so the inner loop
    // computes each twiddle base address with one `add` instead of
    // a `mul` per iteration.
    let m2 = m << 1
    let m3 = m2 + m
    let m4 = m << 2
    let m5 = m4 + m
    let m6 = m3 << 1
    var b = 0
    while b < n {
      // SIMD2 path over consecutive `k` pairs.
      var k = 0
      while k < mPairs {
        let i0 = b + k
        let i1 = i0 + m
        let i2 = i1 + m
        let i3 = i2 + m
        let i4 = i3 + m
        let i5 = i4 + m
        let i6 = i5 + m
        // 6 twiddles loaded as SIMD2 from contiguous (k, k+1) entries.
        let mk = m + k
        let m2k = m2 + k
        let m3k = m3 + k
        let m4k = m4 + k
        let m5k = m5 + k
        let m6k = m6 + k
        let t1R = ldSIMD2(twRe, mk)
        let t1I = ldSIMD2(twIm, mk)
        let t2R = ldSIMD2(twRe, m2k)
        let t2I = ldSIMD2(twIm, m2k)
        let t3R = ldSIMD2(twRe, m3k)
        let t3I = ldSIMD2(twIm, m3k)
        let t4R = ldSIMD2(twRe, m4k)
        let t4I = ldSIMD2(twIm, m4k)
        let t5R = ldSIMD2(twRe, m5k)
        let t5I = ldSIMD2(twIm, m5k)
        let t6R = ldSIMD2(twRe, m6k)
        let t6I = ldSIMD2(twIm, m6k)
        let r0r = ldSIMD2(workRe, i0)
        let r0i = ldSIMD2(workIm, i0)
        let r1rRaw = ldSIMD2(workRe, i1)
        let r1iRaw = ldSIMD2(workIm, i1)
        let r2rRaw = ldSIMD2(workRe, i2)
        let r2iRaw = ldSIMD2(workIm, i2)
        let r3rRaw = ldSIMD2(workRe, i3)
        let r3iRaw = ldSIMD2(workIm, i3)
        let r4rRaw = ldSIMD2(workRe, i4)
        let r4iRaw = ldSIMD2(workIm, i4)
        let r5rRaw = ldSIMD2(workRe, i5)
        let r5iRaw = ldSIMD2(workIm, i5)
        let r6rRaw = ldSIMD2(workRe, i6)
        let r6iRaw = ldSIMD2(workIm, i6)
        // Apply twiddles (complex multiply per lane).
        let r1r = r1rRaw * t1R - r1iRaw * t1I
        let r1i = r1rRaw * t1I + r1iRaw * t1R
        let r2r = r2rRaw * t2R - r2iRaw * t2I
        let r2i = r2rRaw * t2I + r2iRaw * t2R
        let r3r = r3rRaw * t3R - r3iRaw * t3I
        let r3i = r3rRaw * t3I + r3iRaw * t3R
        let r4r = r4rRaw * t4R - r4iRaw * t4I
        let r4i = r4rRaw * t4I + r4iRaw * t4R
        let r5r = r5rRaw * t5R - r5iRaw * t5I
        let r5i = r5rRaw * t5I + r5iRaw * t5R
        let r6r = r6rRaw * t6R - r6iRaw * t6I
        let r6i = r6rRaw * t6I + r6iRaw * t6R
        // Pair sums/diffs.
        let ps16R = r1r + r6r
        let ps16I = r1i + r6i
        let pd16R = r1r - r6r
        let pd16I = r1i - r6i
        let ps25R = r2r + r5r
        let ps25I = r2i + r5i
        let pd25R = r2r - r5r
        let pd25I = r2i - r5i
        let ps34R = r3r + r4r
        let ps34I = r3i + r4i
        let pd34R = r3r - r4r
        let pd34I = r3i - r4i
        // Build outputs incrementally to keep type checker fast.
        // O[0] = v0 + s16 + s25 + s34
        var o0r = r0r
        o0r += ps16R
        o0r += ps25R
        o0r += ps34R
        var o0i = r0i
        o0i += ps16I
        o0i += ps25I
        o0i += ps34I
        stSIMD2(workRe, i0, o0r)
        stSIMD2(workIm, i0, o0i)
        // O[1]: (w1, w2, w3)
        var o1r = r0r
        o1r += w1R * ps16R
        o1r -= w1I * pd16I
        o1r += w2R * ps25R
        o1r -= w2I * pd25I
        o1r += w3R * ps34R
        o1r -= w3I * pd34I
        var o1i = r0i
        o1i += w1R * ps16I
        o1i += w1I * pd16R
        o1i += w2R * ps25I
        o1i += w2I * pd25R
        o1i += w3R * ps34I
        o1i += w3I * pd34R
        stSIMD2(workRe, i1, o1r)
        stSIMD2(workIm, i1, o1i)
        // O[2]: (w2, conj(w3), conj(w1))
        var o2r = r0r
        o2r += w2R * ps16R
        o2r -= w2I * pd16I
        o2r += w3R * ps25R
        o2r -= (-w3I) * pd25I
        o2r += w1R * ps34R
        o2r -= (-w1I) * pd34I
        var o2i = r0i
        o2i += w2R * ps16I
        o2i += w2I * pd16R
        o2i += w3R * ps25I
        o2i += (-w3I) * pd25R
        o2i += w1R * ps34I
        o2i += (-w1I) * pd34R
        stSIMD2(workRe, i2, o2r)
        stSIMD2(workIm, i2, o2i)
        // O[3]: (w3, conj(w1), w2)
        var o3r = r0r
        o3r += w3R * ps16R
        o3r -= w3I * pd16I
        o3r += w1R * ps25R
        o3r -= (-w1I) * pd25I
        o3r += w2R * ps34R
        o3r -= w2I * pd34I
        var o3i = r0i
        o3i += w3R * ps16I
        o3i += w3I * pd16R
        o3i += w1R * ps25I
        o3i += (-w1I) * pd25R
        o3i += w2R * ps34I
        o3i += w2I * pd34R
        stSIMD2(workRe, i3, o3r)
        stSIMD2(workIm, i3, o3i)
        // O[4]: (conj(w3), w1, conj(w2))
        var o4r = r0r
        o4r += w3R * ps16R
        o4r -= (-w3I) * pd16I
        o4r += w1R * ps25R
        o4r -= w1I * pd25I
        o4r += w2R * ps34R
        o4r -= (-w2I) * pd34I
        var o4i = r0i
        o4i += w3R * ps16I
        o4i += (-w3I) * pd16R
        o4i += w1R * ps25I
        o4i += w1I * pd25R
        o4i += w2R * ps34I
        o4i += (-w2I) * pd34R
        stSIMD2(workRe, i4, o4r)
        stSIMD2(workIm, i4, o4i)
        // O[5]: (conj(w2), w3, w1)
        var o5r = r0r
        o5r += w2R * ps16R
        o5r -= (-w2I) * pd16I
        o5r += w3R * ps25R
        o5r -= w3I * pd25I
        o5r += w1R * ps34R
        o5r -= w1I * pd34I
        var o5i = r0i
        o5i += w2R * ps16I
        o5i += (-w2I) * pd16R
        o5i += w3R * ps25I
        o5i += w3I * pd25R
        o5i += w1R * ps34I
        o5i += w1I * pd34R
        stSIMD2(workRe, i5, o5r)
        stSIMD2(workIm, i5, o5i)
        // O[6]: (conj(w1), conj(w2), conj(w3))
        var o6r = r0r
        o6r += w1R * ps16R
        o6r -= (-w1I) * pd16I
        o6r += w2R * ps25R
        o6r -= (-w2I) * pd25I
        o6r += w3R * ps34R
        o6r -= (-w3I) * pd34I
        var o6i = r0i
        o6i += w1R * ps16I
        o6i += (-w1I) * pd16R
        o6i += w2R * ps25I
        o6i += (-w2I) * pd25R
        o6i += w3R * ps34I
        o6i += (-w3I) * pd34R
        stSIMD2(workRe, i6, o6r)
        stSIMD2(workIm, i6, o6i)
        k += 2
      }
      // Scalar tail for odd `m` (or the entire stage when m == 1).
      while k < m {
        let i0 = b + k
        let i1 = i0 + m
        let i2 = i1 + m
        let i3 = i2 + m
        let i4 = i3 + m
        let i5 = i4 + m
        let i6 = i5 + m
        // Outer-stage twiddles on samples 1..6. Reuse the cached
        // `m2..m6` from above so the twiddle base addresses are
        // single `+` ops instead of `Int` multiplies with overflow
        // traps.
        let t1R = twRe[m + k]
        let t1I = twIm[m + k]
        let t2R = twRe[m2 + k]
        let t2I = twIm[m2 + k]
        let t3R = twRe[m3 + k]
        let t3I = twIm[m3 + k]
        let t4R = twRe[m4 + k]
        let t4I = twIm[m4 + k]
        let t5R = twRe[m5 + k]
        let t5I = twIm[m5 + k]
        let t6R = twRe[m6 + k]
        let t6I = twIm[m6 + k]
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
        // Build pair sums/diffs with conjugate-symmetric partners.
        // {1,6}: W^1, W^6 = W^-1 → coef pair (w1, conj(w1))
        // {2,5}: W^2, W^5 = W^-2 → (w2, conj(w2))
        // {3,4}: W^3, W^4 = W^-3 → (w3, conj(w3))
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
        // O[0] = v0 + sum-of-all
        workRe[i0] = v0r + s16R + s25R + s34R
        workIm[i0] = v0i + s16I + s25I + s34I
        // For each output O[k], the coefficient of pair {1,6} is W^(k·1),
        // for {2,5} is W^(2k), for {3,4} is W^(3k). For k > 3, use that
        // W^(jk) modulo 7 wraps and yields conjugates.
        //
        //   k=1: (1,2,3) → (w1, w2, w3)
        //   k=2: (2,4,6) → (w2, conj(w3), conj(w1))
        //   k=3: (3,6,9 mod 7=2) → (w3, conj(w1), w2)
        //   k=4: (4,8 mod 7=1, 12 mod 7=5) → (conj(w3), w1, conj(w2))
        //   k=5: (5, 10 mod 7=3, 15 mod 7=1) → (conj(w2), w3, w1)
        //   k=6: (6, 12 mod 7=5, 18 mod 7=4) → (conj(w1), conj(w2), conj(w3))
        //
        // Generic: pairResult.re = zR·sumR - zI·diffI;  .im = zR·sumI + zI·diffR.
        // Output the sum across the three pairs, plus v0.
        // O[1]: (w1, w2, w3)
        workRe[i1] =
          v0r + w1R * s16R - w1I * d16I + w2R * s25R - w2I * d25I + w3R * s34R - w3I * d34I
        workIm[i1] =
          v0i + w1R * s16I + w1I * d16R + w2R * s25I + w2I * d25R + w3R * s34I + w3I * d34R
        // O[2]: (w2, conj(w3), conj(w1))
        workRe[i2] =
          v0r + w2R * s16R - w2I * d16I
          + w3R * s25R - (-w3I) * d25I
          + w1R * s34R - (-w1I) * d34I
        workIm[i2] =
          v0i + w2R * s16I + w2I * d16R
          + w3R * s25I + (-w3I) * d25R
          + w1R * s34I + (-w1I) * d34R
        // O[3]: (w3, conj(w1), w2)
        workRe[i3] =
          v0r + w3R * s16R - w3I * d16I
          + w1R * s25R - (-w1I) * d25I
          + w2R * s34R - w2I * d34I
        workIm[i3] =
          v0i + w3R * s16I + w3I * d16R
          + w1R * s25I + (-w1I) * d25R
          + w2R * s34I + w2I * d34R
        // O[4]: (conj(w3), w1, conj(w2))
        workRe[i4] =
          v0r + w3R * s16R - (-w3I) * d16I
          + w1R * s25R - w1I * d25I
          + w2R * s34R - (-w2I) * d34I
        workIm[i4] =
          v0i + w3R * s16I + (-w3I) * d16R
          + w1R * s25I + w1I * d25R
          + w2R * s34I + (-w2I) * d34R
        // O[5]: (conj(w2), w3, w1)
        workRe[i5] =
          v0r + w2R * s16R - (-w2I) * d16I
          + w3R * s25R - w3I * d25I
          + w1R * s34R - w1I * d34I
        workIm[i5] =
          v0i + w2R * s16I + (-w2I) * d16R
          + w3R * s25I + w3I * d25R
          + w1R * s34I + w1I * d34R
        // O[6]: (conj(w1), conj(w2), conj(w3))
        workRe[i6] =
          v0r + w1R * s16R - (-w1I) * d16I
          + w2R * s25R - (-w2I) * d25I
          + w3R * s34R - (-w3I) * d34I
        workIm[i6] =
          v0i + w1R * s16I + (-w1I) * d16R
          + w2R * s25I + (-w2I) * d25R
          + w3R * s34I + (-w3I) * d34R
        k += 1
      }
      b += blockSize
    }
  }
}
