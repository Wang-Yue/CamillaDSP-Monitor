// PCM → DoP encoder. Inverse of `DoPDecoder`: converts a chunk of PCM
// audio at the carrier rate into DSD-over-PCM, in place. For each input
// frame we
//   1. interpolate 16× to the DSD rate using a 511-tap β=11 Kaiser-windowed
//      polyphase sinc (same shape as the decoder, normalized per phase
//      for unit DC gain),
//   2. modulate the oversampled signal with a per-channel sigma-delta
//      modulator (`sdm-4` / `sdm-5` / `sdm-6` picked by DSD rate), and
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

import DSPAudio
import DSPLogging
import Foundation

public final class DoPEncoder: @unchecked Sendable {
  private let logger = Logger(label: "dsp.dop.encode")

  public static let phases = 16  // 16× DSD oversampling per PCM frame
  private static let realTaps = 511
  private static let numTaps = 512  // padded to multiple of phases
  private static let subFilterTaps = numTaps / phases  // 32 — must be power of 2
  private static let fifoMask = subFilterTaps - 1

  /// Carrier sample rates that produce a valid DoP stream — DSD64/128/256
  /// over the 44.1 kHz and 48 kHz rate families. Anything outside this set
  /// can't be DoP-encoded: the modulator's filter table only has entries
  /// for these specific DSD rates, and a downstream DAC won't recognize
  /// the marker pattern at any other carrier rate.
  public static let supportedCarrierRates: Set<Int> = [
    176_400, 352_800, 705_600,  // 44.1 kHz family — DSD64 / 128 / 256
    192_000, 384_000, 768_000,  // 48 kHz   family — DSD64 / 128 / 256
  ]

  private final class ChannelState: @unchecked Sendable {
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
  public let enabled: Bool
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
  /// - Parameter filterName: noise-shaper filter name (e.g. "sdm-4", "sdm-5", "sdm-6", or "auto").
  /// - Parameter cutoffHz: passband cutoff of the interpolation filter
  ///   (default 20 kHz). Lower values trade ultrasonic passband for
  ///   sharper image rejection. Ignored when `enabled` is false.
  public init(
    channels: Int, sampleRate: Double, outputDoP: Bool, filterName: String = "auto",
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
    let selectedFilter =
      filterName == "auto" ? DoPEncoder.pickSDMFilter(dsdRate: dsdRate) : filterName
    var states: [ChannelState] = []
    states.reserveCapacity(channels)
    for _ in 0..<channels {
      guard
        let modulator = SigmaDeltaModulator(
          filterName: selectedFilter, freq: UInt32(dsdRate.rounded()))
      else {
        // The supported-rate gate above guarantees a matching SDM filter
        // exists in the table; reaching this would mean the table itself
        // is missing an entry for a rate we claim to support.
        fatalError("DoPEncoder: SigmaDeltaModulator missing filter at \(dsdRate) Hz")
      }
      states.append(
        ChannelState(fifoSize: DoPEncoder.subFilterTaps, modulator: modulator))
    }
    self.channelStates = states
    logger.info("DoP encoder active at %d Hz carrier (%s)", .int(rateInt), .string(selectedFilter))
  }

  deinit {
    coeffs.deinitialize(count: DoPEncoder.phases * DoPEncoder.subFilterTaps)
    coeffs.deallocate()
  }

  /// Encode the chunk's `validFrames` PCM samples into DoP, in place.
  /// No-op when `enabled` is `false`, the chunk is empty, or the channel
  /// count doesn't match what the encoder was constructed with.
  public func encode(chunk: inout AudioChunk) {
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

    let fifoPtr = state.fifo
    var pos = state.fifoPos
    var marker = state.marker

    for t in 0..<frames {
      // Push the new PCM sample into both halves of the polyphase FIR's history.
      let sampleVal = Double(buf[t])
      fifoPtr[pos] = sampleVal
      fifoPtr[pos + nTaps] = sampleVal

      // For each of the 16 oversampled phases, compute the interpolated
      // sample and feed it through the SDM. Phase p=0 is the oldest
      // sample within this frame's 16-sample window and ends up in the
      // MSB of the packed word; phase p=15 is the newest and ends up in
      // the LSB. This matches the bit ordering used by `DoPDecoder`.
      var word: UInt16 = 0
      let baseIdx = pos + 1
      for p in 0..<16 {
        let coeffOffset = p * 32
        let coeffP = coeffPtr + coeffOffset
        let fifoP = fifoPtr + baseIdx

        let c0 = UnsafeRawPointer(coeffP).loadUnaligned(as: SIMD4<Double>.self)
        let f0 = UnsafeRawPointer(fifoP).loadUnaligned(as: SIMD4<Double>.self)
        let c1 = UnsafeRawPointer(coeffP + 4).loadUnaligned(as: SIMD4<Double>.self)
        let f1 = UnsafeRawPointer(fifoP + 4).loadUnaligned(as: SIMD4<Double>.self)
        let c2 = UnsafeRawPointer(coeffP + 8).loadUnaligned(as: SIMD4<Double>.self)
        let f2 = UnsafeRawPointer(fifoP + 8).loadUnaligned(as: SIMD4<Double>.self)
        let c3 = UnsafeRawPointer(coeffP + 12).loadUnaligned(as: SIMD4<Double>.self)
        let f3 = UnsafeRawPointer(fifoP + 12).loadUnaligned(as: SIMD4<Double>.self)
        let c4 = UnsafeRawPointer(coeffP + 16).loadUnaligned(as: SIMD4<Double>.self)
        let f4 = UnsafeRawPointer(fifoP + 16).loadUnaligned(as: SIMD4<Double>.self)
        let c5 = UnsafeRawPointer(coeffP + 20).loadUnaligned(as: SIMD4<Double>.self)
        let f5 = UnsafeRawPointer(fifoP + 20).loadUnaligned(as: SIMD4<Double>.self)
        let c6 = UnsafeRawPointer(coeffP + 24).loadUnaligned(as: SIMD4<Double>.self)
        let f6 = UnsafeRawPointer(fifoP + 24).loadUnaligned(as: SIMD4<Double>.self)
        let c7 = UnsafeRawPointer(coeffP + 28).loadUnaligned(as: SIMD4<Double>.self)
        let f7 = UnsafeRawPointer(fifoP + 28).loadUnaligned(as: SIMD4<Double>.self)

        var sumVec = c0 * f0
        sumVec += c1 * f1
        sumVec += c2 * f2
        sumVec += c3 * f3
        sumVec += c4 * f4
        sumVec += c5 * f5
        sumVec += c6 * f6
        sumVec += c7 * f7
        let acc = sumVec.sum()

        let dsd = modulator.sdmSample(acc * 0.5)

        if dsd > 0 {
          word |= UInt16(1) << (15 - p)
        }
      }

      // 24-bit DoP container: marker in bits 23..16, DSD word in bits 15..0.
      // Sign-extend from int24 and normalize back to ±1.0 float for the
      // playback backend, which will re-quantize to the device format
      // (must be S24 or S32 to preserve the bit pattern).
      let val24: UInt32 = (UInt32(marker) << 16) | UInt32(word)
      let intVal: Int32 = Int32(bitPattern: val24 << 8) >> 8
      buf[t] = PrcFmt(Double(intVal) / 8388608.0)

      marker = (marker == 0x05) ? 0xFA : 0x05
      pos = (pos &+ 1) & mask
    }

    state.fifoPos = pos
    state.marker = marker
  }

  // MARK: - SDM filter selection

  /// Pick a noise-shaper aggressive enough for the target DSD rate.
  /// `sdm-4` is sufficient for DSD64; higher DSD rates afford the
  /// stability margin needed by the higher-order shapers.
  private static func pickSDMFilter(dsdRate: Double) -> String {
    let mult = Int((dsdRate / 44100.0).rounded())
    switch mult {
    case ..<128: return "sdm-4"
    case ..<256: return "sdm-5"
    default: return "sdm-6"
    }
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

    func besselI0(_ x: Double) -> Double {
      var sum = 1.0
      var denominator = 1.0
      var i = 1.0
      while i < 25.0 {
        denominator *= i
        let term = pow(x / 2.0, i) / denominator
        sum += term * term
        i += 1.0
      }
      return sum
    }

    let i0Beta = besselI0(beta)
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
      let windowVal = besselI0(beta * widx) / i0Beta
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
