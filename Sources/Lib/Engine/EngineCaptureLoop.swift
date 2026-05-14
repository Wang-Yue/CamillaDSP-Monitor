// Capture thread body. One instance per engine run; the thread
// closure invokes `run()` exactly once and returns when the shared
// `shouldStop` flag is set or a stop reason is reported.
//
// State ownership
// ---------------
// All mutable state — the working chunk, the silence counter, the
// stall watchdog — lives inside the loop instance and is touched
// only by the capture thread. Cross-thread communication happens
// exclusively through the injected `EngineSharedState`.
//
// Audio-thread invariants
// -----------------------
//   * No allocations in the steady-state. The working `AudioChunk`
//     is constructed once at init.
//   * No locks. Coordination uses the shared SPSC queue + semaphore.
//   * No `Date()` / `gettimeofday`. The watchdog uses
//     `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` (vDSO read on
//     Darwin — no syscall).

import DSPAudio
import DSPBackend
import DSPConfig
import DSPDoP
import DSPLogging
import Foundation
import Synchronization

/// `@unchecked Sendable` is a *transfer* vouch, not a *share*
/// vouch: the instance is safe to cross the Thread spawn boundary
/// because exactly one thread (the loop thread) ever touches it
/// after `run()` is invoked. The mutable state — the working
/// `AudioChunk`, the silence counter, the stall watchdog — has no
/// internal synchronisation and is *not* safe to use from multiple
/// threads concurrently.
final class EngineCaptureLoop: @unchecked Sendable {
  private let logger = Logger(label: "camilladsp.capture")

  private let shared: EngineSharedState
  private let stateMachine: EngineStateMachine
  private let capture: CaptureBackend
  private let playback: PlaybackBackend
  private let processingParams: ProcessingParameters
  private var dopDecoder: DoPDecoder

  private let chunkSize: Int
  private let channels: Int

  /// Hooked stop callback. Invoked when capture decides the engine
  /// must shut down (format change / capture error / stall). The
  /// host wires this to `DSPEngineCore.stop(reason:)` so the once-CAS
  /// teardown runs exactly once even when several signals fire
  /// concurrently.
  private let onStop: (ProcessingStopReason) -> Void

  init(
    shared: EngineSharedState,
    stateMachine: EngineStateMachine,
    capture: CaptureBackend,
    playback: PlaybackBackend,
    processingParams: ProcessingParameters,
    dopDecoder: DoPDecoder,
    chunkSize: Int,
    channels: Int,
    samplerate: Int,
    silenceThresholdDb: Double,
    silenceTimeoutSeconds: Double,
    onStop: @escaping (ProcessingStopReason) -> Void
  ) {
    self.shared = shared
    self.stateMachine = stateMachine
    self.capture = capture
    self.playback = playback
    self.processingParams = processingParams
    self.dopDecoder = dopDecoder
    self.chunkSize = chunkSize
    self.channels = channels
    self.onStop = onStop
    self.silenceCounter = SilenceCounter(
      thresholdDb: silenceThresholdDb,
      timeoutSeconds: silenceTimeoutSeconds,
      samplerate: samplerate,
      chunksize: chunkSize
    )
  }

  // Loop-private state.
  private var silenceCounter: SilenceCounter
  private var watchdog = StallWatchdog(timeoutSeconds: 0.5)

  func run() {
    logger.info("Capture thread started")
    var chunkPool = RoundRobinChunkPool(
      capacity: shared.capturedQueue.capacity + 4,
      frames: chunkSize,
      channels: channels
    )

    while !shared.shouldStop.load(ordering: .acquiring) {
      // Surface a HAL-level sample-rate change before doing any
      // more work. A user (or another app) flipping the device
      // rate in Audio MIDI Setup invalidates the AudioUnit's
      // configured format; the cleanest recovery is to stop and
      // let the host rebuild.
      if let rate = capture.pendingRateChange {
        logger.info("Capture device rate changed to %f Hz; stopping engine", .double(rate))
        onStop(.captureFormatChange(Int(rate.rounded())))
        return
      }
      if let rate = playback.pendingRateChange {
        logger.info("Playback device rate changed to %f Hz; stopping engine", .double(rate))
        onStop(.playbackFormatChange(Int(rate.rounded())))
        return
      }

      do {
        var chunk = chunkPool.next()
        let gotData = try capture.read(frames: chunkSize, into: &chunk)
        if !gotData {
          handleEmptyRead()
          continue
        }
        watchdog.onSuccessfulRead { logger.info("Capture recovered from stall") }

        // Decode DoP in place before computing capture levels so the
        // monitoring meters reflect the actual decoded audio rather
        // than the carrier waveform with its high-frequency marker
        // bytes (which would otherwise show a tiny ~0.04 amplitude
        // floor).
        do {
          _ = try dopDecoder.detectAndProcess(chunk: &chunk)
        } catch {
          logger.error("DoP decode error: %s", .string("\(error)"))
        }

        let loudestPeak = processingParams.updateCaptureLevels(from: chunk)

        // Update silence detector with the loudest channel's peak.
        // We only flip when the value actually changes to avoid
        // hammering the atomic from the audio thread.
        let desired = silenceCounter.update(signalPeakDb: loudestPeak)
        let current = stateMachine.state
        if desired != current {
          stateMachine.setState(desired)
        }

        // Enqueue for processing. The lock-free SPSC queue is
        // bounded; on overflow we drop the chunk rather than
        // allocate. We bump an atomic counter instead of calling
        // the logger — formatting / locking inside the logger is
        // a poor fit for the audio-priority capture thread, and
        // particularly bad precisely when the system is already
        // overloaded.
        if stateMachine.state != .paused {
          if !shared.capturedQueue.enqueue(chunk) {
            shared.capturedDropCounter.wrappingAdd(1, ordering: .relaxed)
          }
          shared.capturedSemaphore.signal()
        }
      } catch {
        logger.error("Capture error: %s", .string("\(error)"))
        onStop(.captureError("\(error)"))
        return
      }
    }
    logger.info("Capture thread stopped")
  }

  /// Slow path when `capture.read` returns no data (e.g. transient
  /// HAL hiccup). Trip the watchdog after `stallTimeout` seconds and
  /// back off briefly so we don't spin.
  private func handleEmptyRead() {
    if shared.shouldStop.load(ordering: .acquiring) { return }
    if watchdog.tickEmptyRead() {
      stateMachine.setState(.stalled)
      logger.warning("Capture device stalled — no data for %fs", .double(watchdog.timeoutSeconds))
    }
    // Back off by 20ms when officially stalled to conserve CPU power.
    // Otherwise, back off by 1ms during short transient HAL hiccups.
    let backoff = stateMachine.state == .stalled ? 0.020 : 0.001
    Thread.sleep(forTimeInterval: backoff)
  }
}

// MARK: - SilenceCounter

/// Counts consecutive silent chunks against a dB threshold and
/// reports back the desired engine state. `update(signalPeakDb:)`
/// returns `.paused` once silence has persisted for at least the
/// configured timeout, `.running` otherwise.
///
/// Disabled when `timeoutSeconds <= 0` — in that case `update`
/// always returns `.running`.
struct SilenceCounter {
  private let limitChunks: Int
  private let thresholdDb: Double
  private var silentChunks: Int = 0

  init(thresholdDb: Double, timeoutSeconds: Double, samplerate: Int, chunksize: Int) {
    self.thresholdDb = thresholdDb
    if timeoutSeconds > 0, chunksize > 0 {
      self.limitChunks = Int((timeoutSeconds * Double(samplerate) / Double(chunksize)).rounded())
    } else {
      self.limitChunks = 0
    }
  }

  /// Feed the next chunk's loudest channel peak (dB). Returns the
  /// engine state the capture loop should drive to.
  mutating func update(signalPeakDb: Double) -> ProcessingState {
    guard limitChunks > 0 else { return .running }
    if signalPeakDb > thresholdDb {
      silentChunks = 0
      return .running
    }
    if silentChunks < limitChunks {
      silentChunks += 1
    }
    return silentChunks >= limitChunks ? .paused : .running
  }
}

// MARK: - StallWatchdog

/// Detects a hung capture device — `read` returning no data for
/// longer than `timeoutSeconds` consecutively. The watchdog records
/// the monotonic time of the most recent successful read and reports
/// `true` exactly once per stall (subsequent ticks return `false`
/// until the next successful read clears the flag).
///
/// Backed by `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` — a vDSO
/// read on Darwin, no syscall, suitable for invocation on every
/// audio-thread iteration.
struct StallWatchdog {
  let timeoutSeconds: Double
  private var lastSuccessNs: UInt64
  private var triggered: Bool = false

  init(timeoutSeconds: Double) {
    self.timeoutSeconds = timeoutSeconds
    self.lastSuccessNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
  }

  /// Called when `capture.read` returns no data. Returns `true` the
  /// first time the empty-read window crosses `timeoutSeconds`; a
  /// repeated call before the next successful read returns `false`.
  mutating func tickEmptyRead() -> Bool {
    if triggered { return false }
    let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
    let elapsed = Double(now &- lastSuccessNs) / 1_000_000_000.0
    if elapsed > timeoutSeconds {
      triggered = true
      return true
    }
    return false
  }

  /// Called after a successful read. If the watchdog had previously
  /// fired, invoke `onRecovery` once before clearing the flag.
  mutating func onSuccessfulRead(_ onRecovery: () -> Void) {
    lastSuccessNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
    if triggered {
      triggered = false
      onRecovery()
    }
  }
}
