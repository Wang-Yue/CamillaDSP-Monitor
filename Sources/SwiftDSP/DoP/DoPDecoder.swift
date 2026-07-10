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

import Foundation

final class DoPDecoder {
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

    let fifo: UnsafeMutablePointer<UInt8>
    let fifoSize: Int
    var fifoPos: Int = 0

    init(fifoSize: Int) {
      self.fifoSize = fifoSize
      self.fifo = .allocate(capacity: fifoSize * 2)
      self.fifo.initialize(repeating: DoPDecoder.silenceByte, count: fifoSize * 2)
    }

    deinit {
      fifo.deinitialize(count: fifoSize * 2)
      fifo.deallocate()
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

  private(set) var isDoPActive = false

  /// - Parameters:
  ///   - channels: Number of audio channels.
  ///   - sampleRate: The PCM sample rate (carrier rate).
  ///   - bypassDoP: If true, DoP detection is disabled and input is passed through.
  ///   - cutoffHz: Passband cutoff of the post-DSD lowpass (default 20 kHz).
  ///     Lower values trade ultrasonic passband for higher SINAD.
  init(
    channels: Int, sampleRate: Double, bypassDoP: Bool = true, cutoffHz: Double = 20_000.0
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
  func detectAndProcess(chunk: inout AudioChunk) throws -> Bool {
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
    guard let base = buf.baseAddress else { return }

    let fifo = state.fifo
    var pos = state.fifoPos

    for t in 0..<frames {
      let raw = base[t]

      var marker: UInt8 = 0
      var dsdWord: UInt16 = 0

      if state.containerKnown {
        if state.is32BitContainer {
          let scaled = raw * 2147483648.0
          let val32: Int32
          if scaled >= 2147483647.0 {
            val32 = .max
          } else if scaled <= -2147483648.0 {
            val32 = .min
          } else {
            val32 = Int32(lrint(scaled))
          }
          marker = UInt8((UInt32(bitPattern: val32) >> 16) & 0xFF)
          dsdWord = UInt16(UInt32(bitPattern: val32) & 0xFFFF)
        } else {
          let scaled = raw * 8388608.0
          let val24: Int32
          if scaled >= 8388607.0 {
            val24 = 8_388_607
          } else if scaled <= -8388608.0 {
            val24 = -8_388_608
          } else {
            val24 = Int32(lrint(scaled))
          }
          marker = UInt8((UInt32(bitPattern: val24) >> 16) & 0xFF)
          dsdWord = UInt16(UInt32(bitPattern: val24) & 0xFFFF)
        }
      } else {
        let scaled32 = raw * 2147483648.0
        let val32: Int32
        if scaled32 >= 2147483647.0 {
          val32 = .max
        } else if scaled32 <= -2147483648.0 {
          val32 = .min
        } else {
          val32 = Int32(lrint(scaled32))
        }
        let marker32 = UInt8((UInt32(bitPattern: val32) >> 16) & 0xFF)

        let scaled24 = raw * 8388608.0
        let val24: Int32
        if scaled24 >= 8388607.0 {
          val24 = 8_388_607
        } else if scaled24 <= -8388608.0 {
          val24 = -8_388_608
        } else {
          val24 = Int32(lrint(scaled24))
        }
        let marker24 = UInt8((UInt32(bitPattern: val24) >> 16) & 0xFF)

        if marker32 == 0x05 || marker32 == 0xFA {
          state.is32BitContainer = true
          marker = marker32
          dsdWord = UInt16(UInt32(bitPattern: val32) & 0xFFFF)
        } else if marker24 == 0x05 || marker24 == 0xFA {
          state.is32BitContainer = false
          marker = marker24
          dsdWord = UInt16(UInt32(bitPattern: val24) & 0xFFFF)
        } else {
          marker = marker24
          dsdWord = UInt16(UInt32(bitPattern: val24) & 0xFFFF)
        }
      }

      let isMarkerValid = (marker == 0x05 || marker == 0xFA)
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
            let totalSize = state.fifoSize * 2
            for i in 0..<totalSize {
              fifo[i] = DoPDecoder.silenceByte
            }
            pos = 0
          }
        }
      }

      let push = valid || state.isActive
      if push {
        let dsdHi = UInt8((dsdWord >> 8) & 0xFF)
        let dsdLo = UInt8(dsdWord & 0xFF)
        fifo[pos] = dsdHi
        fifo[pos + state.fifoSize] = dsdHi
        pos = (pos &+ 1) & mask
        fifo[pos] = dsdLo
        fifo[pos + state.fifoSize] = dsdLo
        pos = (pos &+ 1) & mask
      }

      if state.isActive {
        var acc0 = 0.0
        var acc1 = 0.0
        var acc2 = 0.0
        var acc3 = 0.0
        let readPtr = pos - 1 + state.fifoSize
        for i in stride(from: 0, to: ncTables, by: 4) {
          let b0 = Int(fifo[readPtr - i])
          let b1 = Int(fifo[readPtr - (i + 1)])
          let b2 = Int(fifo[readPtr - (i + 2)])
          let b3 = Int(fifo[readPtr - (i + 3)])
          acc0 += tables[i * 256 + b0]
          acc1 += tables[(i + 1) * 256 + b1]
          acc2 += tables[(i + 2) * 256 + b2]
          acc3 += tables[(i + 3) * 256 + b3]
        }
        let acc = acc0 + acc1 + acc2 + acc3
        base[t] = Double(acc * 2.0)
      }
    }

    state.fifoPos = pos
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

    let i0Beta = Double.besselI0(beta)
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
      let windowVal = Double.besselI0(beta * widx) / i0Beta
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
