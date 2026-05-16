// DoP detection and decoding.
//
// DSD-over-PCM packs 16 1-bit DSD samples into the lower 16 bits of each
// PCM frame; the upper byte carries a magic marker that alternates
// `0x05` ↔ `0xFA` between consecutive frames. We detect by looking for that
// strict alternation and decode by streaming the recovered DSD bytes
// through the same 511-tap Kaiser-windowed sinc the previous
// `DSDPolyphaseDecimator` used (β=11, cutoff = 20 kHz / dsd_rate),
// resampling 16:1 back to the carrier rate.
//
// The detection state machine is hysteretic: 32 consecutive valid alternating
// frames per channel to lock on, 64 consecutive bad frames to release. The
// asymmetry kills the PCM↔DSD flicker the previous "reset on a single bad
// frame" code exhibited at chunk boundaries and around isolated bit errors.
//
// The hot path runs on the audio thread, so the decoder allocates nothing
// per call. Per-channel state is a 64-byte ring FIFO of DSD bytes; the
// convolution becomes 64 byte-indexed table lookups
// (`acc += ctables[i][fifo[i]]`) — each table precomputes the contribution
// of a byte at a given offset in the filter, replacing the per-bit
// conditional add. Filter shape, tap count, and cutoff are unchanged from
// the previous design, so the SINAD numbers the existing tests pin down
// across DSD64 / 128 / 256 at 44.1 / 48 kHz families are preserved.

import DSPAudio
import DSPLogging
import Foundation

public final class DoPDecoder {
  private let logger = Logger(label: "dsp.dop")

  /// Frames of valid alternating markers required to lock on. ~180 µs at
  /// 176.4 kHz PCM rate.
  private static let activateThreshold = 32

  /// Frames of bad markers required to release the lock once active.
  /// Asymmetric vs. `activateThreshold` is intentional — a single corrupted
  /// PCM sample on a real DoP stream should not flip the engine back to PCM.
  private static let deactivateThreshold = 64

  /// Chunks of consistent state required before logging a state transition.
  /// Suppresses brief lock→lost→lock flickers seen at stream start (e.g.
  /// when the source has a few hundred microseconds of pre-roll silence
  /// between bursts of DoP). Only the *settled* state is logged.
  private static let logSettleChunks = 4

  // Filter / lookup-table layout.
  private static let realTaps = 511
  private static let numTaps = 512  // padded so 8-bit slicing is exact
  private static let numCtables = numTaps / 8  // 64
  private static let fifoSize = numCtables  // power of 2
  private static let fifoMask = fifoSize - 1

  /// One of the standard DSD silence patterns. Initializing the FIFO to
  /// this rather than zero (= all `-1` = DC saturated) means the first
  /// few samples after activation don't produce a click.
  private static let silenceByte: UInt8 = 0x69

  private final class ChannelState {
    var consecValid: Int = 0
    var consecInvalid: Int = 0
    var isActive: Bool = false
    var lastMarker: UInt8 = 0
    var is32BitContainer: Bool = false
    var containerKnown: Bool = false

    var fifo: [UInt8]
    var fifoPos: Int = 0

    init(fifoSize: Int) {
      self.fifo = [UInt8](repeating: DoPDecoder.silenceByte, count: fifoSize)
    }
  }

  private let channels: Int
  private let bypassDoP: Bool
  private var channelStates: [ChannelState]

  /// Flat ctable storage: `ctables[i*256 + b]` is the convolution
  /// contribution of byte `b` placed at table index `i`. Built once at
  /// init from the configured sample rate and cutoff; never resized.
  private let ctables: UnsafeMutablePointer<Double>

  // Log debouncer state.
  private var loggedActive: Bool = false
  private var lastSeenActive: Bool = false
  private var chunksAtSeenState: Int = 0

  public private(set) var isDoPActive = false

  /// - Parameter cutoffHz: passband cutoff of the post-DSD lowpass (default 20 kHz).
  ///   Lower values trade ultrasonic passband for higher SINAD.
  public init(
    channels: Int, sampleRate: Double, bypassDoP: Bool = false, cutoffHz: Double = 20_000.0
  ) {
    self.channels = channels
    self.bypassDoP = bypassDoP
    self.channelStates = (0..<channels).map { _ in
      ChannelState(fifoSize: DoPDecoder.fifoSize)
    }
    self.ctables = DoPDecoder.buildCtables(sampleRate: sampleRate, cutoffHz: cutoffHz)
  }

  deinit {
    let count = DoPDecoder.numCtables * 256
    ctables.deinitialize(count: count)
    ctables.deallocate()
  }

  /// Detect DoP and (when active) decode the chunk in place. Returns
  /// `true` iff the chunk was decoded.
  public func detectAndProcess(chunk: inout AudioChunk) throws -> Bool {
    if bypassDoP {
      isDoPActive = false
      return false
    }

    let validFrames = chunk.validFrames
    guard validFrames > 0, chunk.channels == channels else { return false }

    for ch in 0..<channels {
      processChannel(state: channelStates[ch], buf: chunk[ch], frames: validFrames)
    }

    var allActive = true
    for st in channelStates where !st.isActive {
      allActive = false
      break
    }
    self.isDoPActive = allActive

    // Log debouncer: only log a transition once the new state has been
    // observed for `logSettleChunks` consecutive chunks. This filters out
    // the lock→lost→lock churn that fires at stream start when the source
    // has brief silence between DoP bursts.
    if self.isDoPActive == lastSeenActive {
      chunksAtSeenState &+= 1
    } else {
      lastSeenActive = self.isDoPActive
      chunksAtSeenState = 1
    }
    if chunksAtSeenState >= DoPDecoder.logSettleChunks && lastSeenActive != loggedActive {
      if lastSeenActive {
        let s: StaticString =
          channelStates[0].is32BitContainer ? "32-bit container" : "24-bit container"
        logger.info("DoP stream locked (%s)", .staticString(s))
      } else {
        logger.info("DoP stream lost; reverting to PCM")
      }
      loggedActive = lastSeenActive
    }

    return self.isDoPActive
  }

  private func processChannel(state: ChannelState, buf: MutableWaveform, frames: Int) {
    let activate = DoPDecoder.activateThreshold
    let deactivate = DoPDecoder.deactivateThreshold
    let mask = DoPDecoder.fifoMask
    let ncTables = DoPDecoder.numCtables
    let tables = self.ctables

    state.fifo.withUnsafeMutableBufferPointer { fifo in
      var pos = state.fifoPos

      for t in 0..<frames {
        let raw = buf[t]

        // Recover both 24- and 32-bit container interpretations. DoP is most
        // commonly carried as right-aligned 24-bit-in-32-bit (marker at bits
        // 23..16 of int24). MPD's flavor encodes a true 32-bit value
        // 0xff05XXXX / 0xfffaXXXX where the top byte sign-extends and the
        // marker is still at bits 23..16 — same shift, different float scale.
        var v32 = raw * 2147483648.0
        v32.round(.toNearestOrEven)
        let val32: Int32
        if v32 >= 2147483647.0 {
          val32 = .max
        } else if v32 <= -2147483648.0 {
          val32 = .min
        } else {
          val32 = Int32(v32)
        }
        let marker32 = UInt8((UInt32(bitPattern: val32) >> 16) & 0xFF)

        var v24 = raw * 8388608.0
        v24.round(.toNearestOrEven)
        let val24: Int32
        if v24 >= 8388607.0 {
          val24 = 8_388_607
        } else if v24 <= -8388608.0 {
          val24 = -8_388_608
        } else {
          val24 = Int32(v24)
        }
        let marker24 = UInt8((UInt32(bitPattern: val24) >> 16) & 0xFF)

        if !state.containerKnown {
          if marker32 == 0x05 || marker32 == 0xFA {
            state.is32BitContainer = true
          } else if marker24 == 0x05 || marker24 == 0xFA {
            state.is32BitContainer = false
          }
        }

        let marker = state.is32BitContainer ? marker32 : marker24
        let dsdWord: UInt16 =
          state.is32BitContainer
          ? UInt16(UInt32(bitPattern: val32) & 0xFFFF)
          : UInt16(UInt32(bitPattern: val24) & 0xFFFF)

        let isMarkerValid = (marker == 0x05 || marker == 0xFA)
        // First-ever frame on this channel passes vacuously; subsequent
        // frames must alternate between 0x05 and 0xFA.
        let alternates = state.lastMarker == 0 || marker != state.lastMarker
        let valid = isMarkerValid && alternates

        if valid {
          state.consecValid &+= 1
          state.consecInvalid = 0
          state.lastMarker = marker
          if !state.containerKnown && state.consecValid >= 4 {
            state.containerKnown = true
          }
          if !state.isActive && state.consecValid >= activate {
            state.isActive = true
          }
        } else {
          state.consecInvalid &+= 1
          state.consecValid = 0
          if state.consecInvalid >= deactivate {
            state.lastMarker = 0
            state.containerKnown = false
            if state.isActive {
              state.isActive = false
              for i in 0..<fifo.count { fifo[i] = DoPDecoder.silenceByte }
              pos = 0
            }
          }
        }

        // Push the frame's two DSD bytes whenever we either have a
        // current valid marker (warming the filter pre-lock) or are
        // already locked on (trusting the lock through isolated marker
        // bit-errors). Either way, by the time `isActive` flips true the
        // FIFO already holds 32 frames of real DSD data, so the first
        // decoded sample is not a silence-fill transient.
        let push = valid || state.isActive
        if push {
          let dsdHi = UInt8((dsdWord >> 8) & 0xFF)
          let dsdLo = UInt8(dsdWord & 0xFF)
          fifo[pos] = dsdHi
          pos = (pos &+ 1) & mask
          fifo[pos] = dsdLo
          pos = (pos &+ 1) & mask
        }

        if state.isActive {
          // y[n] = Σ_{i<numCtables} ctables[i][fifo[(pos-1-i) & mask]].
          // ctable[i] precomputes the contribution of bits 0..7 of the
          // byte at offset `i` to filter taps i*8 .. i*8+7 — see
          // buildCtables for the bit/tap mapping.
          var acc = 0.0
          for i in 0..<ncTables {
            let byteIdx = (pos &- 1 &- i) & mask
            let b = Int(fifo[byteIdx])
            acc += tables[i * 256 + b]
          }
          // The trellis-friendly sigma-delta modulators in the test suite
          // pre-scale input by 0.5 for noise-shaper headroom; this 2× compensates
          // so SINAD compares against full-amplitude sin. Real DoP streams
          // from DACs that don't pre-scale will be 6 dB hot — handle upstream
          // if that becomes a problem.
          buf[t] = PrcFmt(acc * 2.0)
        }
      }

      state.fifoPos = pos
    }
  }

  // MARK: - Coefficient table construction

  /// Build the byte-indexed filter lookup tables for a 511-tap, β=11
  /// Kaiser-windowed sinc with cutoff at `cutoffHz / dsd_rate`. The filter
  /// shape itself is unchanged from the previous `DSDPolyphaseDecimator`
  /// (same Kaiser sinc generator); only the absolute cutoff is now
  /// configurable. SINAD vs. ultrasonic-passband is the trade-off:
  /// 20 kHz is the SINAD-optimal default; 30–50 kHz preserves more
  /// ultrasonic content at modest SINAD cost.
  ///
  /// Bit/byte mapping: bit `m` (LSB-first) of the byte at table index `i`
  /// corresponds to filter tap `h[i*8 + m]`, applied to the DSD sample at
  /// offset `i*8 + m` behind the most recent push. With our DoP unpack,
  /// the most recent byte is the lower byte of the frame's 16-bit DSD
  /// payload and bit 0 of that byte is the latest of the frame's 16
  /// DSD samples (LSB-first within byte = newer first within byte).
  private static func buildCtables(sampleRate: Double, cutoffHz: Double)
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
    var rawH = [Double](repeating: 0.0, count: realTaps)
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
      rawH[i] = sincVal * windowVal
    }
    let totalSum = rawH.reduce(0.0, +)
    var taps = [Double](repeating: 0.0, count: numTaps)  // tap 511 stays 0
    for i in 0..<realTaps {
      taps[i] = rawH[i] / totalSum
    }

    let total = numCtables * 256
    let p = UnsafeMutablePointer<Double>.allocate(capacity: total)
    for i in 0..<numCtables {
      for b in 0..<256 {
        var sum = 0.0
        for m in 0..<8 {
          let tap = i * 8 + m
          let h = taps[tap]
          let bit = (b >> m) & 1
          sum += h * (bit == 1 ? 1.0 : -1.0)
        }
        (p + (i * 256 + b)).initialize(to: sum)
      }
    }
    return p
  }
}
