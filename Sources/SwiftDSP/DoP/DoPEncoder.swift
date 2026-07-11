// PCM → DoP encoder. Inverse of `DoPDecoder`: converts a chunk of PCM
// audio at the carrier rate into DSD-over-PCM, in place. For each input
// frame we
//   1. interpolate 16× to the DSD rate using a 511-tap β=11 Kaiser-windowed
//      polyphase sinc (same shape as the decoder, normalized per phase
//      for unit DC gain),
//   2. modulate the oversampled signal with a per-channel sigma-delta
//      modulator (using the configured `SDMFilter`, defaulting to `sdm-6`), and
//   3. pack the 16 resulting DSD bits into the lower 16 bits of a 24-bit
//      container, with an alternating `0x05` / `0xFA` marker in the
//      upper byte.
//
// The encoded chunk satisfies the strict-alternation detection state
// machine in `DoPDecoder` and round-trips through any DAC that natively
// understands DoP. To preserve the bit pattern through CoreAudio the
// playback format must be S24 or S32 (F32 will quantize the marker
// away); the encoder itself just emits float-normalised 24-bit values
// and trusts the playback backend to forward them losslessly.
//
// SDM state per channel is carried by an embedded `SigmaDeltaModulator`;
// the polyphase coefficient table is shared across channels and built
// once at init.

import Accelerate
import DSPConfig
import Foundation

final class DoPEncoder {
  private let logger = Logger(label: "dsp.dop.encode")

  static let phases = 16  // 16× DSD oversampling per PCM frame
  private static let realTaps = 511
  private static let numTaps = 512  // padded to multiple of phases
  private static let subFilterTaps = numTaps / phases  // 32 — must be power of 2
  private static let fifoMask = subFilterTaps - 1

  /// Carrier sample rates that produce a valid DoP stream — DSD64/128/256
  /// over the 44.1 kHz and 48 kHz rate families. Anything outside this set
  /// can't be DoP-encoded: the modulator's filter table only has entries
  /// for these specific DSD rates, and a downstream DAC won't recognize
  /// the marker pattern at any other carrier rate.
  static let supportedCarrierRates: Set<Int> = [
    176_400, 352_800, 705_600,  // 44.1 kHz family — DSD64 / 128 / 256
    192_000, 384_000, 768_000,  // 48 kHz   family — DSD64 / 128 / 256
  ]

  private final class ChannelState {
    let fifo: UnsafeMutablePointer<Double>
    var fifoPos: Int = 0
    var marker: UInt8 = 0x05
    let modulator: SigmaDeltaModulator
    private let fifoSize: Int

    init(fifoSize: Int, modulator: SigmaDeltaModulator) {
      self.fifoSize = fifoSize
      self.fifo = UnsafeMutablePointer<Double>.allocate(capacity: fifoSize * 2)
      self.fifo.initialize(repeating: 0.0, count: fifoSize * 2)
      self.modulator = modulator
    }

    deinit {
      fifo.deinitialize(count: fifoSize * 2)
      fifo.deallocate()
    }
  }

  private let channels: Int
  /// `true` iff the constructor was asked to encode AND the carrier rate
  /// is in `supportedCarrierRates`. `encode(...)` is an unconditional
  /// no-op when this is `false`.
  let enabled: Bool
  private let channelStates: [ChannelState]

  /// Polyphase coefficient table laid out as `coeffs[phase * subFilterTaps + tap]`.
  /// Each phase is normalized to unit DC gain; with a constant input sequence
  /// the interpolated output equals the input value, so the SDM input scale
  /// matches the PCM input scale. Built unconditionally — at unsupported
  /// rates the table is harmless dead weight (~4 KB) but keeping the
  /// allocation unconditional simplifies the deinit path.
  private let coeffs: UnsafeMutablePointer<Double>

  /// Construct an encoder. Always succeeds, but only actually encodes
  /// when `outputDoP` is `true` *and* `sampleRate` is one of
  /// `supportedCarrierRates`. The mismatched case is logged once at
  /// construction and reduces `encode(...)` to a no-op.
  ///
  /// - Parameters:
  ///   - channels: Number of audio channels.
  ///   - sampleRate: The PCM sample rate (carrier rate).
  ///   - outputDoP: If true, enables DoP encoding.
  ///   - filterName: Noise-shaper filter name (defaults to `.sdm6`).
  ///   - cutoffHz: Passband cutoff of the interpolation filter (default 20 kHz).
  ///     Lower values trade ultrasonic passband for sharper image rejection. Ignored when `enabled` is false.
  init(
    channels: Int, sampleRate: Double, outputDoP: Bool, filterName: SDMFilter = .sdm6,
    cutoffHz: Double = 20_000.0
  ) {
    self.channels = channels
    self.coeffs = DoPEncoder.buildCoeffs(sampleRate: sampleRate, cutoffHz: cutoffHz)

    let rateInt = Int(sampleRate.rounded())
    let supported = DoPEncoder.supportedCarrierRates.contains(rateInt)
    self.enabled = outputDoP && supported

    if outputDoP && !supported {
      logger.warning(
        "DoP output requested but %d Hz is not a supported DSD carrier rate (need 176400/352800/705600 Hz for the 44.1 kHz family or 192000/384000/768000 Hz for the 48 kHz family); bypassing encoder",
        .int(rateInt))
      self.channelStates = []
      return
    }

    guard outputDoP else {
      self.channelStates = []
      return
    }

    let dsdRate = sampleRate * 16.0
    let selectedFilter = filterName
    var states: [ChannelState] = []
    states.reserveCapacity(channels)
    for _ in 0..<channels {
      let modulator = SigmaDeltaModulator(
        filterName: selectedFilter, freq: UInt32(dsdRate.rounded()))
      states.append(
        ChannelState(fifoSize: DoPEncoder.subFilterTaps, modulator: modulator))
    }
    self.channelStates = states
    logger.info(
      "DoP encoder active at %d Hz carrier (%s)", .int(rateInt), .string(selectedFilter.rawValue))
  }

  deinit {
    coeffs.deinitialize(count: DoPEncoder.phases * DoPEncoder.subFilterTaps)
    coeffs.deallocate()
  }

  /// Encode the chunk's `validFrames` PCM samples into DoP, in place.
  /// No-op when `enabled` is `false`, the chunk is empty, or the channel
  /// count doesn't match what the encoder was constructed with.
  func encode(chunk: inout AudioChunk) {
    guard enabled else { return }
    let n = chunk.validFrames
    guard n > 0, chunk.channels == channels else { return }
    for ch in 0..<channels {
      encodeChannel(state: channelStates[ch], buf: chunk[ch], frames: n)
    }
  }

  private func encodeChannel(state: ChannelState, buf: MutableWaveform, frames: Int) {
    let mask = DoPEncoder.fifoMask
    let nTaps = DoPEncoder.subFilterTaps
    let coeffPtr = self.coeffs
    let modulator = state.modulator
    guard let base = buf.baseAddress else { return }

    let fifoPtr = state.fifo
    var pos = state.fifoPos
    var marker = state.marker

    for t in 0..<frames {
      // Push the new PCM sample into both halves of the polyphase FIR's history.
      let sampleVal = Double(base[t])
      fifoPtr[pos] = sampleVal
      fifoPtr[pos + nTaps] = sampleVal

      // Load history once for all 16 oversampled phases.
      let baseIdx = pos + 1
      let fifoP = fifoPtr + baseIdx

      let f0 = UnsafeRawPointer(fifoP).loadUnaligned(as: SIMD4<Double>.self)
      let f1 = UnsafeRawPointer(fifoP + 4).loadUnaligned(as: SIMD4<Double>.self)
      let f2 = UnsafeRawPointer(fifoP + 8).loadUnaligned(as: SIMD4<Double>.self)
      let f3 = UnsafeRawPointer(fifoP + 12).loadUnaligned(as: SIMD4<Double>.self)
      let f4 = UnsafeRawPointer(fifoP + 16).loadUnaligned(as: SIMD4<Double>.self)
      let f5 = UnsafeRawPointer(fifoP + 20).loadUnaligned(as: SIMD4<Double>.self)
      let f6 = UnsafeRawPointer(fifoP + 24).loadUnaligned(as: SIMD4<Double>.self)
      let f7 = UnsafeRawPointer(fifoP + 28).loadUnaligned(as: SIMD4<Double>.self)

      var word: UInt16 = 0
      for p in stride(from: 0, to: 16, by: 4) {
        let coeffOffset0 = p * 32
        let coeffP0 = coeffPtr + coeffOffset0
        let c0_0 = UnsafeRawPointer(coeffP0).loadUnaligned(as: SIMD4<Double>.self)
        let c0_1 = UnsafeRawPointer(coeffP0 + 4).loadUnaligned(as: SIMD4<Double>.self)
        let c0_2 = UnsafeRawPointer(coeffP0 + 8).loadUnaligned(as: SIMD4<Double>.self)
        let c0_3 = UnsafeRawPointer(coeffP0 + 12).loadUnaligned(as: SIMD4<Double>.self)
        let c0_4 = UnsafeRawPointer(coeffP0 + 16).loadUnaligned(as: SIMD4<Double>.self)
        let c0_5 = UnsafeRawPointer(coeffP0 + 20).loadUnaligned(as: SIMD4<Double>.self)
        let c0_6 = UnsafeRawPointer(coeffP0 + 24).loadUnaligned(as: SIMD4<Double>.self)
        let c0_7 = UnsafeRawPointer(coeffP0 + 28).loadUnaligned(as: SIMD4<Double>.self)

        let coeffOffset1 = (p + 1) * 32
        let coeffP1 = coeffPtr + coeffOffset1
        let c1_0 = UnsafeRawPointer(coeffP1).loadUnaligned(as: SIMD4<Double>.self)
        let c1_1 = UnsafeRawPointer(coeffP1 + 4).loadUnaligned(as: SIMD4<Double>.self)
        let c1_2 = UnsafeRawPointer(coeffP1 + 8).loadUnaligned(as: SIMD4<Double>.self)
        let c1_3 = UnsafeRawPointer(coeffP1 + 12).loadUnaligned(as: SIMD4<Double>.self)
        let c1_4 = UnsafeRawPointer(coeffP1 + 16).loadUnaligned(as: SIMD4<Double>.self)
        let c1_5 = UnsafeRawPointer(coeffP1 + 20).loadUnaligned(as: SIMD4<Double>.self)
        let c1_6 = UnsafeRawPointer(coeffP1 + 24).loadUnaligned(as: SIMD4<Double>.self)
        let c1_7 = UnsafeRawPointer(coeffP1 + 28).loadUnaligned(as: SIMD4<Double>.self)

        let coeffOffset2 = (p + 2) * 32
        let coeffP2 = coeffPtr + coeffOffset2
        let c2_0 = UnsafeRawPointer(coeffP2).loadUnaligned(as: SIMD4<Double>.self)
        let c2_1 = UnsafeRawPointer(coeffP2 + 4).loadUnaligned(as: SIMD4<Double>.self)
        let c2_2 = UnsafeRawPointer(coeffP2 + 8).loadUnaligned(as: SIMD4<Double>.self)
        let c2_3 = UnsafeRawPointer(coeffP2 + 12).loadUnaligned(as: SIMD4<Double>.self)
        let c2_4 = UnsafeRawPointer(coeffP2 + 16).loadUnaligned(as: SIMD4<Double>.self)
        let c2_5 = UnsafeRawPointer(coeffP2 + 20).loadUnaligned(as: SIMD4<Double>.self)
        let c2_6 = UnsafeRawPointer(coeffP2 + 24).loadUnaligned(as: SIMD4<Double>.self)
        let c2_7 = UnsafeRawPointer(coeffP2 + 28).loadUnaligned(as: SIMD4<Double>.self)

        let coeffOffset3 = (p + 3) * 32
        let coeffP3 = coeffPtr + coeffOffset3
        let c3_0 = UnsafeRawPointer(coeffP3).loadUnaligned(as: SIMD4<Double>.self)
        let c3_1 = UnsafeRawPointer(coeffP3 + 4).loadUnaligned(as: SIMD4<Double>.self)
        let c3_2 = UnsafeRawPointer(coeffP3 + 8).loadUnaligned(as: SIMD4<Double>.self)
        let c3_3 = UnsafeRawPointer(coeffP3 + 12).loadUnaligned(as: SIMD4<Double>.self)
        let c3_4 = UnsafeRawPointer(coeffP3 + 16).loadUnaligned(as: SIMD4<Double>.self)
        let c3_5 = UnsafeRawPointer(coeffP3 + 20).loadUnaligned(as: SIMD4<Double>.self)
        let c3_6 = UnsafeRawPointer(coeffP3 + 24).loadUnaligned(as: SIMD4<Double>.self)
        let c3_7 = UnsafeRawPointer(coeffP3 + 28).loadUnaligned(as: SIMD4<Double>.self)

        var sum0 = c0_0 * f0
        var sum1 = c1_0 * f0
        var sum2 = c2_0 * f0
        var sum3 = c3_0 * f0

        sum0 += c0_1 * f1
        sum1 += c1_1 * f1
        sum2 += c2_1 * f1
        sum3 += c3_1 * f1

        sum0 += c0_2 * f2
        sum1 += c1_2 * f2
        sum2 += c2_2 * f2
        sum3 += c3_2 * f2

        sum0 += c0_3 * f3
        sum1 += c1_3 * f3
        sum2 += c2_3 * f3
        sum3 += c3_3 * f3

        sum0 += c0_4 * f4
        sum1 += c1_4 * f4
        sum2 += c2_4 * f4
        sum3 += c3_4 * f4

        sum0 += c0_5 * f5
        sum1 += c1_5 * f5
        sum2 += c2_5 * f5
        sum3 += c3_5 * f5

        sum0 += c0_6 * f6
        sum1 += c1_6 * f6
        sum2 += c2_6 * f6
        sum3 += c3_6 * f6

        sum0 += c0_7 * f7
        sum1 += c1_7 * f7
        sum2 += c2_7 * f7
        sum3 += c3_7 * f7

        let acc0 = sum0.sum()
        let acc1 = sum1.sum()
        let acc2 = sum2.sum()
        let acc3 = sum3.sum()

        if modulator.sdmSample(acc0 * 0.5) > 0 {
          word |= UInt16(1) << (15 - p)
        }
        if modulator.sdmSample(acc1 * 0.5) > 0 {
          word |= UInt16(1) << (15 - (p + 1))
        }
        if modulator.sdmSample(acc2 * 0.5) > 0 {
          word |= UInt16(1) << (15 - (p + 2))
        }
        if modulator.sdmSample(acc3 * 0.5) > 0 {
          word |= UInt16(1) << (15 - (p + 3))
        }
      }

      // 24-bit DoP container: marker in bits 23..16, DSD word in bits 15..0.
      // Sign-extend from int24 and normalize back to ±1.0 float for the
      // playback backend, which will re-quantize to the device format
      // (must be S24 or S32 to preserve the bit pattern).
      let val24: UInt32 = (UInt32(marker) << 16) | UInt32(word)
      let intVal: Int32 = (val24 & 0x800000) != 0 ? Int32(bitPattern: val24 | 0xFF000000) : Int32(val24)
      base[t] = Double(Double(intVal) / 8388608.0)

      marker = (marker == 0x05) ? 0xFA : 0x05
      pos = (pos &+ 1) & mask
    }

    state.fifoPos = pos
    state.marker = marker
  }

  // MARK: - Coefficient table construction

  /// Build a polyphase decomposition of a 511-tap β=11 Kaiser-windowed
  /// sinc with cutoff `cutoffHz / dsdRate`. Phase `p` gets taps
  /// `h[m·phases + p]` for `m = 0..<subFilterTaps`; each phase is
  /// normalized to unit DC gain so a constant input passes through
  /// unchanged.
  private static func buildCoeffs(sampleRate: Double, cutoffHz: Double)
    -> UnsafeMutablePointer<Double>
  {
    let beta = 11.0
    let dsdRate = sampleRate * 16.0
    let cutoff = cutoffHz / dsdRate
    let alpha = Double(realTaps - 1) / 2.0

    let i0Beta = Double.besselI0(beta)
    var taps = [Double](repeating: 0.0, count: numTaps)  // tap 511 stays 0
    for i in 0..<realTaps {
      let t = Double(i) - alpha
      let sincVal: Double
      if t == 0 {
        sincVal = 2.0 * cutoff
      } else {
        let angle = 2.0 * Double.pi * cutoff * t
        sincVal = sin(angle) / (Double.pi * t)
      }
      let widx = sqrt(1.0 - pow(t / alpha, 2.0))
      let windowVal = Double.besselI0(beta * widx) / i0Beta
      taps[i] = sincVal * windowVal
    }

    let total = phases * subFilterTaps
    let p = UnsafeMutablePointer<Double>.allocate(capacity: total)
    for ph in 0..<phases {
      var subSum = 0.0
      for m in 0..<subFilterTaps {
        subSum += taps[m * phases + ph]
      }
      let scale = subSum != 0 ? 1.0 / subSum : 0.0
      for m in 0..<subFilterTaps {
        let v = taps[m * phases + ph] * scale
        let storeIdx = ph * subFilterTaps + (subFilterTaps - 1 - m)
        (p + storeIdx).initialize(to: v)
      }
    }
    return p
  }
}
