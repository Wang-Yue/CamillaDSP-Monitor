// Asynchronous windowed-sinc resampler.
//
// Same buffer layout, same `last_index` semantics, same `t_ratio` accumulation,
// same kernel decimation — output samples agree bit-for-bit (modulo the FMA-reduction
// order in the dot product, which is on the order of a few ULPs).
//
// Memory: every internal buffer is sized at init based on `chunkSize` and
// `maxRelativeRatio`. There is **no** dynamic allocation on the hot path.

import Accelerate
import DSPConfig
import Foundation

final class AsyncSincResampler: AudioResampler {
  let channels: Int
  let chunkSize: Int

  // Filter geometry.
  private let sincLen: Int
  private let oversamplingFactor: Int
  private let interpolation: SincInterpolationType

  // ramp toward the target ratio.
  private let baseRatio: Double
  private var resampleRatio: Double
  private var targetRatio: Double
  private var lastIndex: Double  // tracking index
  private let maxRelativeRatio: Double

  // in the interpolator.
  private let sincTable: [Double]

  // Per-channel input buffer. Layout:
  //   [0 .. 2*sincLen)            — history (last 2*sincLen samples of the
  //                                  previous chunk, or zeros initially)
  //   [2*sincLen .. 2*sincLen+chunkSize) — current chunk's data
  //
  // Class-backed `AudioBuffers` so per-channel pointers stay stable for the
  // resampler's lifetime — no Array CoW on the audio thread.
  private let inputBuffer: AudioBuffers

  // Pre-allocated scratch for per-frame `idx` values. Pre-computed once per
  // chunk so the per-channel loops can iterate without repeating the idx
  // accumulation (and without a 2D buffer locking dance).
  private var idxScratch: [Double]
  private var fracScratch: [Double]

  // Maximum output frames the resampler can ever produce in one call. The
  // caller uses this to size the output AudioChunk once at startup.
  let maxOutputFrames: Int

  var ratio: Double { resampleRatio }

  var nextOutputFrames: Int {
    // Calculate output size for input
    // — note `.floor()`, not `.ceil()`. Using ceil
    // here was the source of the off-by-one frame discrepancy.
    let avgRatio = 0.5 * resampleRatio + 0.5 * targetRatio
    let raw = (Double(chunkSize) - Double(sincLen + 1) - lastIndex) * avgRatio
    return Int(raw.rounded(.down))
  }

  init(
    channels: Int, inputRate: Int, outputRate: Int,
    sincLen: Int, oversamplingFactor: Int, interpolation: SincInterpolationType,
    window: WindowFunction, fCutoff: Double?, chunkSize: Int,
    maxRelativeRatio: Double = 1.1
  ) {
    precondition(channels > 0, "channels must be positive")
    precondition(chunkSize > 0, "chunkSize must be positive")
    precondition(maxRelativeRatio >= 1.0, "maxRelativeRatio must be ≥ 1")

    self.channels = channels
    self.chunkSize = chunkSize
    self.baseRatio = Double(outputRate) / Double(inputRate)
    self.sincLen = sincLen
    self.oversamplingFactor = oversamplingFactor
    self.interpolation = interpolation
    self.maxRelativeRatio = maxRelativeRatio

    precondition(
      chunkSize >= 2 * sincLen,
      "chunkSize (\(chunkSize)) must be ≥ 2*sincLen (\(2 * sincLen)) — see buffer-shift contract"
    )

    // Cutoff: computed as f32 then converted to f64 inside
    // `make_sincs` (`asynchro_sinc.rs:96`). Down-sampling scales the cutoff
    // by the ratio so the kernel doesn't pass aliased high frequencies.
    let baseCutoff =
      fCutoff.map { Float($0) } ?? calculateCutoffF32(sincLen: sincLen, window: window)
    let fcF32: Float = baseRatio >= 1.0 ? baseCutoff : baseCutoff * Float(baseRatio)
    let fc = Double(fcF32)
    self.sincTable = makeSincTable(
      sincLen: sincLen, oversamplingFactor: oversamplingFactor, window: window, fc: fc)

    // Input buffer sized to: chunkSize + 2*sincLen. Initial
    // contents are zeros — the first chunk's "history" is silence.
    let bufLen = chunkSize + 2 * sincLen
    self.inputBuffer = AudioBuffers(channels: channels, capacity: bufLen)

    // Initial state.
    self.resampleRatio = baseRatio
    self.targetRatio = baseRatio
    self.lastIndex = -(Double(sincLen) - 1.0)

    // Worst-case output frames: minimum lastIndex (= initial value) × maximum
    // possible ratio (= baseRatio × maxRelativeRatio). +16 slack for the
    // ceil() boundary plus future safety.
    let mostNegativeLastIndex = -(Double(sincLen) - 1.0)
    let maxRatioAbs = baseRatio * maxRelativeRatio
    let rawMax = ((Double(chunkSize) - Double(sincLen + 1) - mostNegativeLastIndex) * maxRatioAbs)
    guard rawMax.isFinite && rawMax >= 0.0 else {
      fatalError("Invalid rawMax: \(rawMax)")
    }
    self.maxOutputFrames = Int(rawMax.rounded(.up)) + 16

    // Pre-allocate scratch for per-frame state.
    self.idxScratch = [Double](repeating: 0, count: maxOutputFrames)
    self.fracScratch = [Double](repeating: 0, count: maxOutputFrames)
  }

  convenience init(
    channels: Int, inputRate: Int, outputRate: Int,
    profile: ResamplerProfile = .balanced, chunkSize: Int,
    maxRelativeRatio: Double = 1.1
  ) {
    let sincLen: Int
    let oversamplingFactor: Int
    let window: WindowFunction
    let interpolation: SincInterpolationType

    switch profile {
    case .veryFast:
      sincLen = 64
      oversamplingFactor = 1024
      window = .hann2
      interpolation = .linear
    case .fast:
      sincLen = 128
      oversamplingFactor = 1024
      window = .blackman2
      interpolation = .linear
    case .balanced:
      sincLen = 192
      oversamplingFactor = 512
      window = .blackmanHarris2
      interpolation = .quadratic
    case .accurate:
      sincLen = 256
      oversamplingFactor = 256
      window = .blackmanHarris2
      interpolation = .cubic
    }

    self.init(
      channels: channels, inputRate: inputRate, outputRate: outputRate,
      sincLen: sincLen, oversamplingFactor: oversamplingFactor,
      interpolation: interpolation, window: window, fCutoff: nil,
      chunkSize: chunkSize, maxRelativeRatio: maxRelativeRatio
    )
  }

  func setRelativeRatio(_ multiplier: Double) {
    let clampedMultiplier = max(0.000001, min(multiplier, maxRelativeRatio))
    targetRatio = baseRatio * clampedMultiplier
  }

  // MARK: - Zero-allocation API

  func process(input: AudioChunk, into output: inout AudioChunk) throws {
    guard input.validFrames == chunkSize else {
      throw ResamplerError.inputSizeMismatch(needed: chunkSize, got: input.validFrames)
    }
    guard output.channels == channels else {
      throw ResamplerError.channelCountMismatch(needed: channels, got: output.channels)
    }
    let outputFrames = nextOutputFrames
    if outputFrames == 0 {
      lastIndex = max(-2.0 * Double(sincLen), lastIndex - Double(chunkSize))
      resampleRatio = targetRatio
      output.validFrames = 0
      return
    }
    if output.frames < outputFrames {
      throw ResamplerError.outputBufferTooSmall(needed: outputFrames, got: output.frames)
    }

    // Shift buffer, write new data, run inner.
    let sLen = sincLen
    let twoSLen = 2 * sLen

    for ch in 0..<channels {
      guard let base = inputBuffer[ch].baseAddress else { continue }
      // Copy [chunkSize..chunkSize + 2*sincLen] to [0..2*sincLen]
      // (shift remaining data to the beginning).
      for i in 0..<twoSLen {
        base[i] = base[chunkSize + i]
      }
    }

    for ch in 0..<channels {
      guard let srcPtr = input[ch].baseAddress,
        let dstPtr = inputBuffer[ch].baseAddress
      else { continue }
      (dstPtr + twoSLen).update(from: srcPtr, count: chunkSize)
    }

    // Pre-compute per-frame `idx` and `fracOffset`. Prologue of
    // the per-frame loop:
    //   t_ratio += t_ratio_increment;
    //   idx += t_ratio;
    //   frac = idx*factor - (idx*factor).floor();
    let tRatioStart = 1.0 / resampleRatio
    let tRatioEnd = 1.0 / targetRatio
    let tRatioIncrement = (tRatioEnd - tRatioStart) / Double(outputFrames)
    let factorD = Double(oversamplingFactor)

    var finalIdx = lastIndex
    idxScratch.withUnsafeMutableBufferPointer { idxBuf in
      fracScratch.withUnsafeMutableBufferPointer { fracBuf in
        guard let idxBase = idxBuf.baseAddress,
          let fracBase = fracBuf.baseAddress
        else { return }
        var tRatio = tRatioStart
        var idx = lastIndex
        for frame in 0..<outputFrames {
          tRatio += tRatioIncrement
          idx += tRatio
          idxBase[frame] = idx
          let scaled = idx * factorD
          fracBase[frame] = scaled - scaled.rounded(.down)
        }
        finalIdx = idx
      }
    }

    // Inner loop, specialised per interpolation mode.
    switch interpolation {
    case .nearest:
      runNearest(outputFrames: outputFrames, output: &output)
    case .linear:
      runLinear(outputFrames: outputFrames, output: &output)
    case .quadratic:
      runQuadratic(outputFrames: outputFrames, output: &output)
    case .cubic:
      runCubic(outputFrames: outputFrames, output: &output)
    }

    // Update state for next chunk.
    lastIndex = max(-2.0 * Double(sincLen), finalIdx - Double(chunkSize))
    resampleRatio = targetRatio
    output.validFrames = outputFrames
  }

  // MARK: - Inner loops

  /// Fetch the (index, subindex) pair for a given (start, frac, sub) triple.
  /// Wrap-around logic.
  @inline(__always)
  private func adjustPoint(start: Int, frac: Int, sub: Int) -> (idx: Int, sub: Int) {
    var index = start
    var subindex = frac + sub
    if subindex < 0 {
      subindex += oversamplingFactor
      index -= 1
    } else if subindex >= oversamplingFactor {
      subindex -= oversamplingFactor
      index += 1
    }
    return (index, subindex)
  }

  private func runNearest(outputFrames: Int, output: inout AudioChunk) {
    let sLen = sincLen
    let twoSLen = 2 * sLen
    let factor = oversamplingFactor
    let factorD = Double(factor)

    sincTable.withUnsafeBufferPointer { tBuf in
      guard let table = tBuf.baseAddress else { return }
      idxScratch.withUnsafeBufferPointer { idxBuf in
        for ch in 0..<channels {
          guard let buf = inputBuffer[ch].baseAddress,
            let out = output[ch].baseAddress
          else { continue }
          for frame in 0..<outputFrames {
            let idx = idxBuf[frame]
            let idxFloor = idx.rounded(.down)
            var startIdx = Int(idxFloor)
            var subindex = Int(((idx - idxFloor) * factorD).rounded())
            if subindex >= factor {
              subindex -= factor
              startIdx += 1
            }

            out[frame] = sincDotProduct(buf + startIdx + twoSLen, table + subindex * sLen, sLen)
          }
        }
      }
    }
  }

  private func runCubic(outputFrames: Int, output: inout AudioChunk) {
    let sLen = sincLen
    let twoSLen = 2 * sLen
    let factor = oversamplingFactor
    let factorD = Double(factor)

    sincTable.withUnsafeBufferPointer { tBuf in
      guard let table = tBuf.baseAddress else { return }
      idxScratch.withUnsafeBufferPointer { idxBuf in
        fracScratch.withUnsafeBufferPointer { fracBuf in
          for ch in 0..<channels {
            guard let buf = inputBuffer[ch].baseAddress,
              let out = output[ch].baseAddress
            else { continue }
            for frame in 0..<outputFrames {
              let idx = idxBuf[frame]
              let idxFloor = idx.rounded(.down)
              let startIdx = Int(idxFloor)
              let frac = Int(((idx - idxFloor) * factorD).rounded(.down))
              let fracOffset = fracBuf[frame]

              // 4 (idx, sub) pairs at sub = -1, 0, 1, 2.
              let p0t = adjustPoint(start: startIdx, frac: frac, sub: -1)
              let p1t = adjustPoint(start: startIdx, frac: frac, sub: 0)
              let p2t = adjustPoint(start: startIdx, frac: frac, sub: 1)
              let p3t = adjustPoint(start: startIdx, frac: frac, sub: 2)

              let p0 = sincDotProduct(buf + p0t.idx + twoSLen, table + p0t.sub * sLen, sLen)
              let p1 = sincDotProduct(buf + p1t.idx + twoSLen, table + p1t.sub * sLen, sLen)
              let p2 = sincDotProduct(buf + p2t.idx + twoSLen, table + p2t.sub * sLen, sLen)
              let p3 = sincDotProduct(buf + p3t.idx + twoSLen, table + p3t.sub * sLen, sLen)

              // interp_cubic (asynchro_sinc.rs:118-128).
              let a0 = p1
              let a1 = -1.0 / 3.0 * p0 - 0.5 * p1 + p2 - 1.0 / 6.0 * p3
              let a2 = 0.5 * (p0 + p2) - p1
              let a3 = 0.5 * (p1 - p2) + 1.0 / 6.0 * (p3 - p0)
              let x = fracOffset
              let x2 = x * x
              let x3 = x2 * x
              out[frame] = a0 + a1 * x + a2 * x2 + a3 * x3
            }
          }
        }
      }
    }
  }

  private func runQuadratic(outputFrames: Int, output: inout AudioChunk) {
    let sLen = sincLen
    let twoSLen = 2 * sLen
    let factor = oversamplingFactor
    let factorD = Double(factor)

    sincTable.withUnsafeBufferPointer { tBuf in
      guard let table = tBuf.baseAddress else { return }
      idxScratch.withUnsafeBufferPointer { idxBuf in
        fracScratch.withUnsafeBufferPointer { fracBuf in
          for ch in 0..<channels {
            guard let buf = inputBuffer[ch].baseAddress,
              let out = output[ch].baseAddress
            else { continue }
            for frame in 0..<outputFrames {
              let idx = idxBuf[frame]
              let idxFloor = idx.rounded(.down)
              let startIdx = Int(idxFloor)
              let frac = Int(((idx - idxFloor) * factorD).rounded(.down))
              let fracOffset = fracBuf[frame]

              // get_nearest_times_3: sub = 0, 1, 2.
              let p0t = adjustPoint(start: startIdx, frac: frac, sub: 0)
              let p1t = adjustPoint(start: startIdx, frac: frac, sub: 1)
              let p2t = adjustPoint(start: startIdx, frac: frac, sub: 2)

              let p0 = sincDotProduct(buf + p0t.idx + twoSLen, table + p0t.sub * sLen, sLen)
              let p1 = sincDotProduct(buf + p1t.idx + twoSLen, table + p1t.sub * sLen, sLen)
              let p2 = sincDotProduct(buf + p2t.idx + twoSLen, table + p2t.sub * sLen, sLen)

              // interp_quad (asynchro_sinc.rs:145-154).
              let a2 = p0 - 2.0 * p1 + p2
              let a1 = -3.0 * p0 + 4.0 * p1 - p2
              let a0 = 2.0 * p0
              let x = fracOffset
              let x2 = x * x
              out[frame] = 0.5 * (a0 + a1 * x + a2 * x2)
            }
          }
        }
      }
    }
  }

  private func runLinear(outputFrames: Int, output: inout AudioChunk) {
    let sLen = sincLen
    let twoSLen = 2 * sLen
    let factor = oversamplingFactor
    let factorD = Double(factor)

    sincTable.withUnsafeBufferPointer { tBuf in
      guard let table = tBuf.baseAddress else { return }
      idxScratch.withUnsafeBufferPointer { idxBuf in
        fracScratch.withUnsafeBufferPointer { fracBuf in
          for ch in 0..<channels {
            guard let buf = inputBuffer[ch].baseAddress,
              let out = output[ch].baseAddress
            else { continue }
            for frame in 0..<outputFrames {
              let idx = idxBuf[frame]
              let idxFloor = idx.rounded(.down)
              let startIdx = Int(idxFloor)
              let frac = Int(((idx - idxFloor) * factorD).rounded(.down))
              let fracOffset = fracBuf[frame]

              // get_nearest_times_2: sub = 0, 1.
              let p0t = adjustPoint(start: startIdx, frac: frac, sub: 0)
              let p1t = adjustPoint(start: startIdx, frac: frac, sub: 1)

              let p0 = sincDotProduct(buf + p0t.idx + twoSLen, table + p0t.sub * sLen, sLen)
              let p1 = sincDotProduct(buf + p1t.idx + twoSLen, table + p1t.sub * sLen, sLen)

              // interp_lin: y0 + x * (y1 - y0).
              out[frame] = p0 + fracOffset * (p1 - p0)
            }
          }
        }
      }
    }
  }
}
