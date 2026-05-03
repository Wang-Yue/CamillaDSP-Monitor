// CamillaDSP-Swift: Asynchronous windowed-sinc resampler.
//
// 1:1 port of rubato's `Async::new_sinc` / `InnerSinc::process` (`asynchro.rs`,
// `asynchro_sinc.rs`, `sinc_interpolator/mod.rs`). Same buffer layout, same
// `last_index` semantics, same `t_ratio` accumulation, same kernel decimation
// — output samples agree with rubato bit-for-bit (modulo the FMA-reduction
// order in the dot product, which is on the order of a few ULPs).
//
// Memory: every internal buffer is sized at init based on `chunkSize` and
// `maxRelativeRatio`. There is **no** dynamic allocation on the hot path.

import Accelerate
import Foundation

public final class AsyncSincResampler: AudioResampler {
  public let channels: Int
  public let chunkSize: Int

  // Filter geometry.
  private let sincLen: Int
  private let halfLen: Int
  private let oversamplingFactor: Int
  private let interpolation: SincInterpolationType

  // Ratio bookkeeping. `resampleRatio` is the value used for the *current*
  // chunk's processing; `targetRatio` is the goal that the next call will
  // ramp toward (mirrors rubato's `resample_ratio` / `target_ratio`).
  private let baseRatio: Double
  private let maxRelativeRatio: Double
  private var resampleRatio: Double
  private var targetRatio: Double
  private var lastIndex: Double  // == rubato's `last_index`

  // Pre-computed windowed sinc table — `table[s * sincLen + p] == sincs[s][p]`
  // in rubato's `ScalarInterpolator`.
  private let sincTable: [Double]

  // Per-channel input buffer. Layout (rubato `asynchro.rs`):
  //   [0 .. 2*sincLen)            — history (last 2*sincLen samples of the
  //                                  previous chunk, or zeros initially)
  //   [2*sincLen .. 2*sincLen+chunkSize) — current chunk's data
  private var inputBuffer: [[Double]]

  // Pre-allocated scratch for per-frame `idx` values. Pre-computed once per
  // chunk so the per-channel loops can iterate without repeating the idx
  // accumulation (and without a 2D buffer locking dance).
  private var idxScratch: [Double]
  private var fracScratch: [Double]

  // Maximum output frames the resampler can ever produce in one call. The
  // caller uses this to size the output AudioChunk once at startup.
  public let maxOutputFrames: Int

  public var ratio: Double { resampleRatio }

  public var nextOutputFrames: Int {
    // Mirror rubato's `calculate_output_size` for `FixedAsync::Input`
    // (`asynchro.rs:382-385`) — note `.floor()`, not `.ceil()`. Using ceil
    // here was the source of the off-by-one frame discrepancy versus rubato.
    let avgRatio = 0.5 * resampleRatio + 0.5 * targetRatio
    let raw = (Double(chunkSize) - Double(sincLen + 1) - lastIndex) * avgRatio
    return Int(raw.rounded(.down))
  }

  public init(
    channels: Int, inputRate: Int, outputRate: Int,
    profile: ResamplerProfile = .balanced, chunkSize: Int,
    maxRelativeRatio: Double = 1.1
  ) {
    precondition(channels > 0, "channels must be positive")
    precondition(chunkSize > 0, "chunkSize must be positive")
    precondition(maxRelativeRatio >= 1.0, "maxRelativeRatio must be ≥ 1")

    self.channels = channels
    self.chunkSize = chunkSize
    self.baseRatio = Double(outputRate) / Double(inputRate)
    self.maxRelativeRatio = maxRelativeRatio

    let window: WindowFunction
    switch profile {
    case .veryFast:
      self.sincLen = 64
      self.oversamplingFactor = 1024
      window = .hann2
      self.interpolation = .linear
    case .fast:
      self.sincLen = 128
      self.oversamplingFactor = 1024
      window = .blackman2
      self.interpolation = .linear
    case .balanced:
      self.sincLen = 192
      self.oversamplingFactor = 512
      window = .blackmanHarris2
      self.interpolation = .quadratic
    case .accurate:
      self.sincLen = 256
      self.oversamplingFactor = 256
      window = .blackmanHarris2
      self.interpolation = .cubic
    }
    self.halfLen = sincLen / 2

    precondition(
      chunkSize >= 2 * sincLen,
      "chunkSize (\(chunkSize)) must be ≥ 2*sincLen (\(2 * sincLen)) — see rubato's buffer-shift contract"
    )

    // Cutoff: rubato computes this as f32 then converts to f64 inside
    // `make_sincs` (`asynchro_sinc.rs:96`). Down-sampling scales the cutoff
    // by the ratio so the kernel doesn't pass aliased high frequencies.
    let baseCutoff = calculateCutoffF32(sincLen: sincLen, window: window)
    let fcF32: Float = baseRatio >= 1.0 ? baseCutoff : baseCutoff * Float(baseRatio)
    let fc = Double(fcF32)
    self.sincTable = makeSincTable(
      sincLen: sincLen, oversamplingFactor: oversamplingFactor, window: window, fc: fc)

    // Input buffer sized to rubato's spec: chunkSize + 2*sincLen. Initial
    // contents are zeros — the first chunk's "history" is silence, matching
    // rubato's `Vec::with_capacity(buffer_len)` zero-init.
    let bufLen = chunkSize + 2 * sincLen
    var bufs: [[Double]] = []
    bufs.reserveCapacity(channels)
    for _ in 0..<channels {
      bufs.append([Double](repeating: 0, count: bufLen))
    }
    self.inputBuffer = bufs

    // Initial state.
    self.resampleRatio = baseRatio
    self.targetRatio = baseRatio
    self.lastIndex = -(Double(sincLen) - 1.0)

    // Worst-case output frames: minimum lastIndex (= initial value) × maximum
    // possible ratio (= baseRatio × maxRelativeRatio). +16 slack for the
    // ceil() boundary plus future safety.
    let mostNegativeLastIndex = -(Double(sincLen) - 1.0)
    let maxRatioAbs = baseRatio * maxRelativeRatio
    self.maxOutputFrames =
      Int(
        ((Double(chunkSize) - Double(sincLen + 1) - mostNegativeLastIndex) * maxRatioAbs)
          .rounded(.up)) + 16

    // Pre-allocate scratch for per-frame state.
    self.idxScratch = [Double](repeating: 0, count: maxOutputFrames)
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

    // Rubato's `process_into_buffer`: shift buffer, write new data, run inner.
    let sLen = sincLen
    let twoSLen = 2 * sLen

    for ch in 0..<channels {
      inputBuffer[ch].withUnsafeMutableBufferPointer { ptr in
        guard let base = ptr.baseAddress else { return }
        // Copy [chunkSize..chunkSize + 2*sincLen] to [0..2*sincLen]
        // (rubato: `buf.copy_within(current_buffer_fill..current_buffer_fill + 2*interp_len, 0)`).
        for i in 0..<twoSLen {
          base[i] = base[chunkSize + i]
        }
      }
    }

    for ch in 0..<channels {
      input.waveforms[ch].withUnsafeBufferPointer { src in
        guard let srcPtr = src.baseAddress else { return }
        inputBuffer[ch].withUnsafeMutableBufferPointer { dst in
          guard let dstPtr = dst.baseAddress else { return }
          let dstPtrShifted = dstPtr + twoSLen
          for i in 0..<chunkSize {
            dstPtrShifted[i] = srcPtr[i]
          }
        }
      }
    }

    // Pre-compute per-frame `idx` and `fracOffset`. Mirrors the prologue of
    // rubato's `InnerSinc::process` per-frame loop:
    //   t_ratio += t_ratio_increment;
    //   idx += t_ratio;
    //   frac = idx*factor - (idx*factor).floor();
    let tRatioStart = 1.0 / resampleRatio
    let tRatioEnd = 1.0 / targetRatio
    let tRatioIncrement = (tRatioEnd - tRatioStart) / Double(outputFrames)
    let factorD = Double(oversamplingFactor)

    var tRatio = tRatioStart
    var idx = lastIndex
    for frame in 0..<outputFrames {
      tRatio += tRatioIncrement
      idx += tRatio
      idxScratch[frame] = idx
      let scaled = idx * factorD
      fracScratch[frame] = scaled - scaled.rounded(.down)
    }
    let finalIdx = idx

    // Inner loop, specialised per interpolation mode.
    switch interpolation {
    case .linear:
      runLinear(outputFrames: outputFrames, output: &output)
    case .quadratic:
      runQuadratic(outputFrames: outputFrames, output: &output)
    case .cubic:
      runCubic(outputFrames: outputFrames, output: &output)
    }

    // Update state for next chunk.
    lastIndex = finalIdx - Double(chunkSize)
    resampleRatio = targetRatio
    output.validFrames = outputFrames
  }

  // MARK: - Inner loops

  /// Fetch the (index, subindex) pair for a given (start, frac, sub) triple.
  /// Mirrors rubato's `get_nearest_times_*` wrap-around logic.
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
            inputBuffer[ch].withUnsafeBufferPointer { iBuf in
              guard let buf = iBuf.baseAddress else { return }
              output.waveforms[ch].withUnsafeMutableBufferPointer { oBuf in
                guard let out = oBuf.baseAddress else { return }
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

                  let p0 = sincDotProduct(
                    buf + p0t.idx + twoSLen, table + p0t.sub * sLen, sLen)
                  let p1 = sincDotProduct(
                    buf + p1t.idx + twoSLen, table + p1t.sub * sLen, sLen)
                  let p2 = sincDotProduct(
                    buf + p2t.idx + twoSLen, table + p2t.sub * sLen, sLen)
                  let p3 = sincDotProduct(
                    buf + p3t.idx + twoSLen, table + p3t.sub * sLen, sLen)

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
            inputBuffer[ch].withUnsafeBufferPointer { iBuf in
              guard let buf = iBuf.baseAddress else { return }
              output.waveforms[ch].withUnsafeMutableBufferPointer { oBuf in
                guard let out = oBuf.baseAddress else { return }
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

                  let p0 = sincDotProduct(
                    buf + p0t.idx + twoSLen, table + p0t.sub * sLen, sLen)
                  let p1 = sincDotProduct(
                    buf + p1t.idx + twoSLen, table + p1t.sub * sLen, sLen)
                  let p2 = sincDotProduct(
                    buf + p2t.idx + twoSLen, table + p2t.sub * sLen, sLen)

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
            inputBuffer[ch].withUnsafeBufferPointer { iBuf in
              guard let buf = iBuf.baseAddress else { return }
              output.waveforms[ch].withUnsafeMutableBufferPointer { oBuf in
                guard let out = oBuf.baseAddress else { return }
                for frame in 0..<outputFrames {
                  let idx = idxBuf[frame]
                  let idxFloor = idx.rounded(.down)
                  let startIdx = Int(idxFloor)
                  let frac = Int(((idx - idxFloor) * factorD).rounded(.down))
                  let fracOffset = fracBuf[frame]

                  // get_nearest_times_2: sub = 0, 1.
                  let p0t = adjustPoint(start: startIdx, frac: frac, sub: 0)
                  let p1t = adjustPoint(start: startIdx, frac: frac, sub: 1)

                  let p0 = sincDotProduct(
                    buf + p0t.idx + twoSLen, table + p0t.sub * sLen, sLen)
                  let p1 = sincDotProduct(
                    buf + p1t.idx + twoSLen, table + p1t.sub * sLen, sLen)

                  // interp_lin: y0 + x * (y1 - y0).
                  out[frame] = p0 + fracOffset * (p1 - p0)
                }
              }
            }
          }
        }
      }
    }
  }
}
