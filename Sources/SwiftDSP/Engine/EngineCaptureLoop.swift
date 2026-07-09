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
//   * No allocations in the steady-state. Audio chunks are obtained
//     from a pre-allocated `RoundRobinChunkPool`.
//   * No locks. Coordination uses the shared SPSC queue + semaphore.
//   * No `Date()` / `gettimeofday`. The watchdog uses
//     `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` (vDSO read on
//     Darwin — no syscall).

import DSPConfig
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
  private let logger = Logger(label: "dsp.capture")

  private let shared: EngineSharedState
  private let stateMachine: EngineStateMachine
  private let capture: CaptureBackend
  private let playback: PlaybackBackend
  private let processingParams: ProcessingParameters
  private var dopDecoder: DoPDecoder

  private let chunkSize: Int
  private let channels: Int
  private let samplerate: Int
  private var lastObservedPendingRate: Double?
  private var lastObservedPlaybackPendingRate: Double?

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
    stopOnRateChange: Bool,
    rateMeasureInterval: Double,
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
    self.samplerate = samplerate
    self.onStop = onStop
    self.silenceCounter = SilenceCounter(
      thresholdDb: silenceThresholdDb,
      timeoutSeconds: silenceTimeoutSeconds,
      samplerate: samplerate,
      chunksize: chunkSize
    )
    self.rateWatcher = SampleRateWatcher(
      targetRate: Double(samplerate),
      measureIntervalSeconds: rateMeasureInterval,
      stopOnRateChange: stopOnRateChange
    )
  }

  // Loop-private state.
  private var silenceCounter: SilenceCounter
  private var watchdog = StallWatchdog(timeoutSeconds: 0.5)
  private var rateWatcher: SampleRateWatcher

  func run() {
    logger.info("Capture thread started")
    setRealtimeThreadPriority(name: "Capture", bufferFrames: chunkSize, sampleRate: samplerate)

    rateWatcher.reset()
    var chunkPool = RoundRobinChunkPool(
      capacity: shared.capturedQueue.capacity + 4,
      frames: chunkSize,
      channels: channels
    )

    while !shared.shouldStop.load(ordering: .acquiring) {
      if stateMachine.state == .paused {
        rateWatcher.reset()
      }

      // Surface a HAL-level sample-rate change before doing any
      // more work. A user (or another app) flipping the device
      // rate in Audio MIDI Setup invalidates the AudioUnit's
      // configured format; the cleanest recovery is to stop
      // unconditionally and let the host rebuild.
      if let rate = capture.pendingRateChange {
        if rate != lastObservedPendingRate {
          lastObservedPendingRate = rate
          logger.warning("Capture device rate changed to %f Hz; stopping engine", .double(rate))
          onStop(.captureFormatChange(Int(rate.rounded())))
          return
        }
      }
      if let rate = playback.pendingRateChange {
        if rate != lastObservedPlaybackPendingRate {
          lastObservedPlaybackPendingRate = rate
          logger.warning("Playback device rate changed to %f Hz; stopping engine", .double(rate))
          onStop(.playbackFormatChange(Int(rate.rounded())))
          return
        }
      }

      do {
        var chunk = chunkPool.next()
        let gotData = try capture.read(frames: chunkSize, into: &chunk)
        if !gotData {
          handleEmptyRead()
          continue
        }
        watchdog.onSuccessfulRead { logger.info("Capture recovered from stall") }

        // Increment captured frames and check sample rate drift.
        if let measuredRate = rateWatcher.tick(frames: chunkSize) {
          if rateWatcher.stopOnRateChange {
            logger.warning(
              "Sample rate change detected (measured: %f Hz, expected: %d Hz); stopping engine",
              .double(measuredRate), .int(samplerate)
            )
            onStop(.captureFormatChange(Int(measuredRate.rounded())))
            return
          } else {
            logger.warning(
              "Sample rate change detected (measured: %f Hz, expected: %d Hz); stop on rate change is disabled, continuing",
              .double(measuredRate), .int(samplerate)
            )
          }
        }

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
          playback.isPaused = (desired == .paused)
        }

        // Enqueue for processing. The lock-free SPSC queue is bounded.
        // On overflow (processing loop is slow), we block the capture thread
        // to yield CPU and propagate backpressure upstream.
        if stateMachine.state != .paused {
          while !shared.capturedQueue.enqueue(chunk) {
            if shared.shouldStop.load(ordering: .acquiring) && stateMachine.stopReason != .done {
              break
            }
            Thread.sleep(forTimeInterval: 0.002)
          }
          shared.capturedSemaphore.signal()
        }
      } catch {
        logger.error("Capture error: %s", .string("\(error)"))
        onStop(.captureError("\(error)"))
        return
      }
    }
    shared.captureFinished.store(true, ordering: .releasing)
    shared.capturedSemaphore.signal()

    logger.info("Capture thread stopped")
  }

  private func handleEmptyRead() {
    if shared.shouldStop.load(ordering: .acquiring) { return }
    if stateMachine.state == .paused {
      watchdog.reset()
      _ = capture.wait(timeout: .now() + .milliseconds(20))
      return
    }
    if watchdog.tickEmptyRead() {
      stateMachine.setState(.stalled)
      logger.warning("Capture device stalled — no data for %fs", .double(watchdog.timeoutSeconds))
    }

    // Wait on the capture device's GCD semaphore for new samples, up to 20ms.
    // This uses a 20ms timeout design, preserving
    // real-time priority propagation under load instead of doing a raw sleep.
    _ = capture.wait(timeout: .now() + .milliseconds(20))
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

  /// Reset the watchdog last success timestamp and clear the triggered flag.
  mutating func reset() {
    lastSuccessNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
    triggered = false
  }
}

// MARK: - SampleRateWatcher

/// Monitors the actual capture rate by counting incoming sample frames
/// over a rolling time window. If the measured rate deviates significantly
/// from the expected target rate, it flags a rate change.
struct SampleRateWatcher {
  let stopOnRateChange: Bool
  private let targetRate: Double
  private let measureIntervalSeconds: Double
  private let tolerance: Double = 0.04
  private let consecLimit: Int = 3

  private var capturedFrames: Int = 0
  private var lastResetNs: UInt64 = 0
  private var deviationCount: Int = 0

  init(targetRate: Double, measureIntervalSeconds: Double, stopOnRateChange: Bool) {
    self.targetRate = targetRate
    self.measureIntervalSeconds = measureIntervalSeconds
    self.stopOnRateChange = stopOnRateChange
  }

  /// Reset the rate watcher stats (e.g. when unpausing or starting).
  mutating func reset() {
    capturedFrames = 0
    lastResetNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
    deviationCount = 0
  }

  /// Record a successfully read chunk of size `frames`.
  /// Returns `measuredRate` (Double) if the measurement interval expired and a rate change was detected,
  /// otherwise returns `nil`.
  mutating func tick(frames: Int) -> Double? {
    if lastResetNs == 0 {
      lastResetNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
    }
    capturedFrames += frames
    let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
    let elapsed = Double(now &- lastResetNs) / 1_000_000_000.0

    guard elapsed >= measureIntervalSeconds else { return nil }

    let measuredRate = Double(capturedFrames) / elapsed
    capturedFrames = 0
    lastResetNs = now

    let minVal = targetRate / (1.0 + tolerance)
    let maxVal = targetRate * (1.0 + tolerance)

    if measuredRate < minVal || measuredRate > maxVal {
      deviationCount += 1
    } else {
      deviationCount = 0
    }

    if deviationCount >= consecLimit {
      return measuredRate
    }
    return nil
  }
}
