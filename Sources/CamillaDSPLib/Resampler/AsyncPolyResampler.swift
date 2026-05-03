// CamillaDSP-Swift: Asynchronous polynomial resampler.
//
// 1:1 port of rubato's `Async::new_poly` / `InnerPoly::process`
// (`asynchro.rs`, `asynchro_fast.rs`). Same buffer layout, same `last_index`
// semantics, same `t_ratio` accumulation, same Newton-form polynomial
// formulas — output samples agree with rubato bit-for-bit.
//
// No anti-aliasing (matches rubato's documented `new_poly` behaviour); for
// quality use `AsyncSincResampler`.
//
// Memory: every internal buffer is sized at init based on `chunkSize` and
// `maxRelativeRatio`. There is **no** dynamic allocation on the hot path.

import Foundation

public final class AsyncPolyResampler: AudioResampler {
  public let channels: Int
  public let chunkSize: Int

  private let interpolation: PolyInterpolation
  private let interpolatorLen: Int  // = nbr_points

  // Ratio bookkeeping.
  private let baseRatio: Double
  private let maxRelativeRatio: Double
  private var resampleRatio: Double
  private var targetRatio: Double
  private var lastIndex: Double  // matches rubato's `last_index`

  // Per-channel input buffer. Layout:
  //   [0 .. 2*nbr_points)            — history (rubato's padding zone)
  //   [2*nbr_points .. 2*nbr_points+chunkSize) — current chunk
  private var inputBuffer: [[Double]]

  // Pre-allocated per-frame scratch. `startIdxScratch` holds the integer
  // floor of `idx`, computed once when `fracScratch` is built — saving the
  // inner loops a `Double.rounded(.down)` + `Int()` cast per output frame.
  private var startIdxScratch: [Int]
  private var fracScratch: [Double]

  public let maxOutputFrames: Int

  public var ratio: Double { resampleRatio }

  public var nextOutputFrames: Int {
    // Mirror rubato's `calculate_output_size` for `FixedAsync::Input`
    // (`asynchro.rs:382-385`) — `.floor()`, not `.ceil()`.
    let avgRatio = 0.5 * resampleRatio + 0.5 * targetRatio
    let raw = (Double(chunkSize) - Double(interpolatorLen + 1) - lastIndex) * avgRatio
    return Int(raw.rounded(.down))
  }

  public init(
    channels: Int, inputRate: Int, outputRate: Int,
    interpolation: PolyInterpolation = .cubic, chunkSize: Int,
    maxRelativeRatio: Double = 1.1
  ) {
    precondition(channels > 0, "channels must be positive")
    precondition(chunkSize > 0, "chunkSize must be positive")
    precondition(maxRelativeRatio >= 1.0, "maxRelativeRatio must be ≥ 1")

    self.channels = channels
    self.chunkSize = chunkSize
    self.baseRatio = Double(outputRate) / Double(inputRate)
    self.maxRelativeRatio = maxRelativeRatio
    self.interpolation = interpolation
    self.interpolatorLen = interpolation.nbrPoints

    precondition(
      chunkSize >= 2 * interpolatorLen,
      "chunkSize (\(chunkSize)) must be ≥ 2*nbrPoints (\(2 * interpolatorLen))"
    )

    let bufLen = chunkSize + 2 * interpolatorLen
    var bufs: [[Double]] = []
    bufs.reserveCapacity(channels)
    for _ in 0..<channels {
      bufs.append([Double](repeating: 0, count: bufLen))
    }
    self.inputBuffer = bufs

    self.resampleRatio = baseRatio
    self.targetRatio = baseRatio
    // Matches rubato's `init_last_index = -(nbr_points/2)` (`asynchro_fast.rs:289`).
    self.lastIndex = -(Double(interpolatorLen) / 2.0)

    let mostNegativeLastIndex = -(Double(interpolatorLen) / 2.0)
    let maxRatioAbs = baseRatio * maxRelativeRatio
    self.maxOutputFrames =
      Int(
        ((Double(chunkSize) - Double(interpolatorLen + 1) - mostNegativeLastIndex)
          * maxRatioAbs).rounded(.up)) + 16

    self.startIdxScratch = [Int](repeating: 0, count: maxOutputFrames)
    self.fracScratch = [Double](repeating: 0, count: maxOutputFrames)
  }

  public func setRelativeRatio(_ multiplier: Double) {
    targetRatio = baseRatio * multiplier
  }

  // MARK: - Zero-allocation API

  public func process(input: AudioChunk, into output: inout AudioChunk) throws {
    guard input.validFrames == chunkSize else {
      throw ResamplerError.inputSizeMismatch(needed: chunkSize, got: input.validFrames)
    }
    guard output.waveforms.count == channels else {
      throw ResamplerError.channelCountMismatch(needed: channels, got: output.waveforms.count)
    }
    let outputFrames = nextOutputFrames
    for ch in 0..<channels {
      guard output.waveforms[ch].count >= outputFrames else {
        throw ResamplerError.outputBufferTooSmall(
          needed: outputFrames, got: output.waveforms[ch].count)
      }
    }

    let nLen = interpolatorLen
    let twoNLen = 2 * nLen

    // Shift buffer + write new chunk (rubato semantics).
    for ch in 0..<channels {
      inputBuffer[ch].withUnsafeMutableBufferPointer { ptr in
        guard let base = ptr.baseAddress else { return }
        base.update(from: base + chunkSize, count: twoNLen)
      }
    }
    for ch in 0..<channels {
      input.waveforms[ch].withUnsafeBufferPointer { src in
        guard let srcPtr = src.baseAddress else { return }
        inputBuffer[ch].withUnsafeMutableBufferPointer { dst in
          guard let dstPtr = dst.baseAddress else { return }
          (dstPtr + twoNLen).update(from: srcPtr, count: chunkSize)
        }
      }
    }

    // Pre-compute idx and frac per output frame.
    let tRatioStart = 1.0 / resampleRatio
    let tRatioEnd = 1.0 / targetRatio
    let tRatioIncrement = (tRatioEnd - tRatioStart) / Double(outputFrames)

    var tRatio = tRatioStart
    var idx = lastIndex
    for frame in 0..<outputFrames {
      tRatio += tRatioIncrement
      idx += tRatio
      let idxFloor = idx.rounded(.down)
      startIdxScratch[frame] = Int(idxFloor)
      fracScratch[frame] = idx - idxFloor
    }
    let finalIdx = idx

    switch interpolation {
    case .linear:
      runLinear(outputFrames: outputFrames, output: &output)
    case .cubic:
      runCubic(outputFrames: outputFrames, output: &output)
    case .quintic:
      runQuintic(outputFrames: outputFrames, output: &output)
    case .septic:
      runSeptic(outputFrames: outputFrames, output: &output)
    }

    lastIndex = finalIdx - Double(chunkSize)
    resampleRatio = targetRatio
    output.validFrames = outputFrames
  }

  // MARK: - Inner loops (rubato's exact polynomial forms)

  private func runLinear(outputFrames: Int, output: inout AudioChunk) {
    let nLen = interpolatorLen  // 2
    let twoNLen = 2 * nLen
    startIdxScratch.withUnsafeBufferPointer { idxBuf in
      fracScratch.withUnsafeBufferPointer { fracBuf in
        guard let fracBase = fracBuf.baseAddress else { return }
        for ch in 0..<channels {
          inputBuffer[ch].withUnsafeBufferPointer { iBuf in
            guard let buf = iBuf.baseAddress else { return }
            output.waveforms[ch].withUnsafeMutableBufferPointer { oBuf in
              guard let out = oBuf.baseAddress else { return }
              for frame in 0..<outputFrames {
                let x = fracBase[frame]
                let base = buf + idxBuf[frame] + twoNLen
                let y0 = base[0]
                let y1 = base[1]
                out[frame] = y0 + x * (y1 - y0)
              }
            }
          }
        }
      }
    }
  }

  private func runCubic(outputFrames: Int, output: inout AudioChunk) {
    let nLen = interpolatorLen  // 4
    let baseOffset = 2 * nLen - 1  // twoNLen + (-1) for cubic alignment
    let pairCount4 = outputFrames & ~3
    let pairCount = outputFrames & ~1
    let inv6 = SIMD2<Double>(repeating: 1.0 / 6.0)

    startIdxScratch.withUnsafeBufferPointer { idxBuf in
      fracScratch.withUnsafeBufferPointer { fracBuf in
        guard let fracBase = fracBuf.baseAddress else { return }
        for ch in 0..<channels {
          inputBuffer[ch].withUnsafeBufferPointer { iBuf in
            guard let buf = iBuf.baseAddress else { return }
            output.waveforms[ch].withUnsafeMutableBufferPointer { oBuf in
              guard let out = oBuf.baseAddress else { return }

              var frame = 0
              while frame < pairCount4 {
                // Pair 1
                let baseA = buf + idxBuf[frame] + baseOffset
                let baseB = buf + idxBuf[frame + 1] + baseOffset
                let yA = UnsafeRawPointer(baseA).loadUnaligned(as: SIMD4<Double>.self)
                let yB = UnsafeRawPointer(baseB).loadUnaligned(as: SIMD4<Double>.self)
                let a1 = SIMD2<Double>(yA.x, yB.x)
                let b1 = SIMD2<Double>(yA.y, yB.y)
                let c1 = SIMD2<Double>(yA.z, yB.z)
                let d1 = SIMD2<Double>(yA.w, yB.w)

                let a0_1 = b1
                let a1_1 = c1 - 0.5 * b1 - inv6 * (a1 + a1 + d1)
                let a2_1 = 0.5 * (a1 + c1) - b1
                let a3_1 = 0.5 * (b1 - c1) + inv6 * (d1 - a1)

                let x1 = ldSIMD2(fracBase, frame)

                var val1 = a3_1 * x1
                val1 += a2_1
                val1 *= x1
                val1 += a1_1
                val1 *= x1
                val1 += a0_1

                // Pair 2
                let baseC = buf + idxBuf[frame + 2] + baseOffset
                let baseD = buf + idxBuf[frame + 3] + baseOffset
                let yC = UnsafeRawPointer(baseC).loadUnaligned(as: SIMD4<Double>.self)
                let yD = UnsafeRawPointer(baseD).loadUnaligned(as: SIMD4<Double>.self)
                let a2 = SIMD2<Double>(yC.x, yD.x)
                let b2 = SIMD2<Double>(yC.y, yD.y)
                let c2 = SIMD2<Double>(yC.z, yD.z)
                let d2 = SIMD2<Double>(yC.w, yD.w)

                let a0_2 = b2
                let a1_2 = c2 - 0.5 * b2 - inv6 * (a2 + a2 + d2)
                let a2_2 = 0.5 * (a2 + c2) - b2
                let a3_2 = 0.5 * (b2 - c2) + inv6 * (d2 - a2)

                let x2 = ldSIMD2(fracBase, frame + 2)

                var val2 = a3_2 * x2
                val2 += a2_2
                val2 *= x2
                val2 += a1_2
                val2 *= x2
                val2 += a0_2

                stSIMD2(out, frame, val1)
                stSIMD2(out, frame + 2, val2)

                frame += 4
              }

              while frame < pairCount {
                let baseA = buf + idxBuf[frame] + baseOffset
                let baseB = buf + idxBuf[frame + 1] + baseOffset
                let yA = UnsafeRawPointer(baseA).loadUnaligned(as: SIMD4<Double>.self)
                let yB = UnsafeRawPointer(baseB).loadUnaligned(as: SIMD4<Double>.self)
                let a = SIMD2<Double>(yA.x, yB.x)
                let b = SIMD2<Double>(yA.y, yB.y)
                let c = SIMD2<Double>(yA.z, yB.z)
                let d = SIMD2<Double>(yA.w, yB.w)

                let a0 = b
                let a1 = c - 0.5 * b - inv6 * (a + a + d)
                let a2 = 0.5 * (a + c) - b
                let a3 = 0.5 * (b - c) + inv6 * (d - a)

                let x = ldSIMD2(fracBase, frame)

                var val = a3 * x
                val += a2
                val *= x
                val += a1
                val *= x
                val += a0

                stSIMD2(out, frame, val)

                frame += 2
              }

              if frame < outputFrames {
                let x = fracBase[frame]
                let base = buf + idxBuf[frame] + baseOffset
                let y0 = base[0]
                let y1 = base[1]
                let y2 = base[2]
                let y3 = base[3]

                let a0 = y1
                let a1 = -1.0 / 3.0 * y0 - 0.5 * y1 + y2 - 1.0 / 6.0 * y3
                let a2 = 0.5 * (y0 + y2) - y1
                let a3 = 0.5 * (y1 - y2) + 1.0 / 6.0 * (y3 - y0)

                out[frame] = a0 + x * (a1 + x * (a2 + x * a3))
              }
            }
          }
        }
      }
    }
  }

  private func runQuintic(outputFrames: Int, output: inout AudioChunk) {
    let nLen = interpolatorLen  // 6
    let baseOffset = 2 * nLen - 2  // twoNLen + (-2) for quintic alignment
    let pairCount = outputFrames & ~1
    let inv120 = SIMD2<Double>(repeating: 1.0 / 120.0)
    startIdxScratch.withUnsafeBufferPointer { idxBuf in
      fracScratch.withUnsafeBufferPointer { fracBuf in
        guard let fracBase = fracBuf.baseAddress else { return }
        for ch in 0..<channels {
          inputBuffer[ch].withUnsafeBufferPointer { iBuf in
            guard let buf = iBuf.baseAddress else { return }
            output.waveforms[ch].withUnsafeMutableBufferPointer { oBuf in
              guard let out = oBuf.baseAddress else { return }

              var frame = 0
              while frame < pairCount {
                let baseA = buf + idxBuf[frame] + baseOffset
                let baseB = buf + idxBuf[frame + 1] + baseOffset
                let y03A = UnsafeRawPointer(baseA).loadUnaligned(as: SIMD4<Double>.self)
                let y45A = UnsafeRawPointer(baseA + 4).loadUnaligned(as: SIMD2<Double>.self)
                let y03B = UnsafeRawPointer(baseB).loadUnaligned(as: SIMD4<Double>.self)
                let y45B = UnsafeRawPointer(baseB + 4).loadUnaligned(as: SIMD2<Double>.self)

                let a = SIMD2<Double>(y03A.x, y03B.x)
                let b = SIMD2<Double>(y03A.y, y03B.y)
                let c = SIMD2<Double>(y03A.z, y03B.z)
                let d = SIMD2<Double>(y03A.w, y03B.w)
                let e = SIMD2<Double>(y45A.x, y45B.x)
                let f = SIMD2<Double>(y45A.y, y45B.y)

                var k5 = -a
                k5 += 5.0 * b
                k5 -= 10.0 * c
                k5 += 10.0 * d
                k5 -= 5.0 * e
                k5 += f
                var k4 = 5.0 * a
                k4 -= 20.0 * b
                k4 += 30.0 * c
                k4 -= 20.0 * d
                k4 += 5.0 * e
                var k3 = -5.0 * a
                k3 -= 5.0 * b
                k3 += 50.0 * c
                k3 -= 70.0 * d
                k3 += 35.0 * e
                k3 -= 5.0 * f
                var k2 = -5.0 * a
                k2 += 80.0 * b
                k2 -= 150.0 * c
                k2 += 80.0 * d
                k2 -= 5.0 * e
                var k1 = 6.0 * a
                k1 -= 60.0 * b
                k1 -= 40.0 * c
                k1 += 120.0 * d
                k1 -= 30.0 * e
                k1 += 4.0 * f
                let k0 = 120.0 * c

                let x = ldSIMD2(fracBase, frame)

                let x2 = x * x
                let x3 = x2 * x
                let x4 = x2 * x2
                let x5 = x2 * x3

                var val = k5 * x5
                val += k4 * x4
                val += k3 * x3
                val += k2 * x2
                val += k1 * x
                val += k0

                let scaled = inv120 * val
                stSIMD2(out, frame, scaled)
                frame += 2
              }
              if frame < outputFrames {
                let x = fracBuf[frame]
                let base = buf + idxBuf[frame] + baseOffset
                let a = base[0]
                let b = base[1]
                let c = base[2]
                let d = base[3]
                let e = base[4]
                let f = base[5]
                let k5 = -a + 5.0 * b - 10.0 * c + 10.0 * d - 5.0 * e + f
                let k4 = 5.0 * a - 20.0 * b + 30.0 * c - 20.0 * d + 5.0 * e
                let k3 = -5.0 * a - 5.0 * b + 50.0 * c - 70.0 * d + 35.0 * e - 5.0 * f
                let k2 = -5.0 * a + 80.0 * b - 150.0 * c + 80.0 * d - 5.0 * e
                let k1 = 6.0 * a - 60.0 * b - 40.0 * c + 120.0 * d - 30.0 * e + 4.0 * f
                let k0 = 120.0 * c

                let x2 = x * x
                let x3 = x2 * x
                let x4 = x2 * x2
                let x5 = x2 * x3
                let val = k5 * x5 + k4 * x4 + k3 * x3 + k2 * x2 + k1 * x + k0

                out[frame] = (1.0 / 120.0) * val
              }
            }
          }
        }
      }
    }
  }

  private func runSeptic(outputFrames: Int, output: inout AudioChunk) {
    let nLen = interpolatorLen  // 8
    let baseOffset = 2 * nLen - 3  // twoNLen + (-3) for septic alignment
    let pairCount = outputFrames & ~1
    let inv5040 = SIMD2<Double>(repeating: 1.0 / 5040.0)
    startIdxScratch.withUnsafeBufferPointer { idxBuf in
      fracScratch.withUnsafeBufferPointer { fracBuf in
        guard let fracBase = fracBuf.baseAddress else { return }
        for ch in 0..<channels {
          inputBuffer[ch].withUnsafeBufferPointer { iBuf in
            guard let buf = iBuf.baseAddress else { return }
            output.waveforms[ch].withUnsafeMutableBufferPointer { oBuf in
              guard let out = oBuf.baseAddress else { return }
              // Septic has 8 coefficient computes (each a 7- or 8-term linear
              // combination) plus an 8-term polynomial — well over 100 ops
              // per output frame. Pairing two frames into NEON's 2-wide
              // double SIMD nearly halves that without doubling the gather
              // cost (just 8 extra scalar loads and 8 SIMD2 packs).
              var frame = 0
              while frame < pairCount {
                let baseA = buf + idxBuf[frame] + baseOffset
                let baseB = buf + idxBuf[frame + 1] + baseOffset
                let y03A = UnsafeRawPointer(baseA).loadUnaligned(as: SIMD4<Double>.self)
                let y47A = UnsafeRawPointer(baseA + 4).loadUnaligned(as: SIMD4<Double>.self)
                let y03B = UnsafeRawPointer(baseB).loadUnaligned(as: SIMD4<Double>.self)
                let y47B = UnsafeRawPointer(baseB + 4).loadUnaligned(as: SIMD4<Double>.self)

                let a = SIMD2<Double>(y03A.x, y03B.x)
                let b = SIMD2<Double>(y03A.y, y03B.y)
                let c = SIMD2<Double>(y03A.z, y03B.z)
                let d = SIMD2<Double>(y03A.w, y03B.w)
                let e = SIMD2<Double>(y47A.x, y47B.x)
                let f = SIMD2<Double>(y47A.y, y47B.y)
                let g = SIMD2<Double>(y47A.z, y47B.z)
                let h = SIMD2<Double>(y47A.w, y47B.w)

                // Build each k_i incrementally so the type checker stays
                // sane on these long SIMD2 sums.
                var k7 = -a
                k7 += 7.0 * b
                k7 -= 21.0 * c
                k7 += 35.0 * d
                k7 -= 35.0 * e
                k7 += 21.0 * f
                k7 -= 7.0 * g
                k7 += h
                var k6 = 7.0 * a
                k6 -= 42.0 * b
                k6 += 105.0 * c
                k6 -= 140.0 * d
                k6 += 105.0 * e
                k6 -= 42.0 * f
                k6 += 7.0 * g
                var k5 = -7.0 * a
                k5 -= 14.0 * b
                k5 += 189.0 * c
                k5 -= 490.0 * d
                k5 += 595.0 * e
                k5 -= 378.0 * f
                k5 += 119.0 * g
                k5 -= 14.0 * h
                var k4 = -35.0 * a
                k4 += 420.0 * b
                k4 -= 1365.0 * c
                k4 += 1960.0 * d
                k4 -= 1365.0 * e
                k4 += 420.0 * f
                k4 -= 35.0 * g
                var k3 = 56.0 * a
                k3 -= 497.0 * b
                k3 += 336.0 * c
                k3 += 1715.0 * d
                k3 -= 3080.0 * e
                k3 += 1869.0 * f
                k3 -= 448.0 * g
                k3 += 49.0 * h
                var k2 = 28.0 * a
                k2 -= 378.0 * b
                k2 += 3780.0 * c
                k2 -= 6860.0 * d
                k2 += 3780.0 * e
                k2 -= 378.0 * f
                k2 += 28.0 * g
                var k1 = -48.0 * a
                k1 += 504.0 * b
                k1 -= 3024.0 * c
                k1 -= 1260.0 * d
                k1 += 5040.0 * e
                k1 -= 1512.0 * f
                k1 += 336.0 * g
                k1 -= 36.0 * h
                let k0 = 5040.0 * d

                let x = ldSIMD2(fracBase, frame)

                // Horner's method
                var val = k7 * x
                val += k6
                val *= x
                val += k5
                val *= x
                val += k4
                val *= x
                val += k3
                val *= x
                val += k2
                val *= x
                val += k1
                val *= x
                val += k0

                let scaled = inv5040 * val
                stSIMD2(out, frame, scaled)
                frame += 2
              }
              if frame < outputFrames {
                let x = fracBuf[frame]
                let base = buf + idxBuf[frame] + baseOffset
                let a = base[0]
                let b = base[1]
                let c = base[2]
                let d = base[3]
                let e = base[4]
                let f = base[5]
                let g = base[6]
                let h = base[7]
                let k7 = -a + 7.0 * b - 21.0 * c + 35.0 * d - 35.0 * e + 21.0 * f - 7.0 * g + h
                let k6 = 7.0 * a - 42.0 * b + 105.0 * c - 140.0 * d + 105.0 * e - 42.0 * f + 7.0 * g
                let k5 =
                  -7.0 * a - 14.0 * b + 189.0 * c - 490.0 * d + 595.0 * e - 378.0 * f + 119.0 * g
                  - 14.0 * h
                let k4 =
                  -35.0 * a + 420.0 * b - 1365.0 * c + 1960.0 * d - 1365.0 * e + 420.0 * f - 35.0
                  * g
                let k3 =
                  56.0 * a - 497.0 * b + 336.0 * c + 1715.0 * d - 3080.0 * e + 1869.0 * f - 448.0
                  * g + 49.0 * h
                let k2 =
                  28.0 * a - 378.0 * b + 3780.0 * c - 6860.0 * d + 3780.0 * e - 378.0 * f + 28.0 * g
                let k1 =
                  -48.0 * a + 504.0 * b - 3024.0 * c - 1260.0 * d + 5040.0 * e - 1512.0 * f + 336.0
                  * g - 36.0 * h
                let k0 = 5040.0 * d

                // Horner's method
                let val =
                  k0 + x * (k1 + x * (k2 + x * (k3 + x * (k4 + x * (k5 + x * (k6 + x * k7))))))

                out[frame] = (1.0 / 5040.0) * val
              }
            }
          }
        }
      }
    }
  }
}
