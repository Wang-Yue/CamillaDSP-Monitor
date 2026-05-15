// Playback thread body. Drains the processing→playback SPSC queue
// and writes each chunk to the playback backend. Also runs the
// rate-adjust control loop: averages the (device-ring + queued-chunks)
// fill level, and once per `adjustPeriod` seconds feeds the average
// to `PIRateController`.
//
// State ownership
// ---------------
// The rate-adjust state — controller, averager, stopwatch, last
// published speed — is local to this loop. The output speed is
// applied either directly to the capture clock (when the capture
// device exposes a tunable clock — BlackHole 0.5.0+) or published
// via `shared.resamplerRatio` so the processing thread picks it up
// on its next chunk.
//
// Audio-thread invariants
// -----------------------
//   * No allocations in the steady state. The controller and
//     averager are constructed once at init; the stopwatch is a
//     plain UInt64 nanosecond timestamp.
//   * No locks. The shared SPSC queue + semaphore carries chunks
//     and wakeups.
//   * The rate-adjust info logger fires at most once per
//     `adjustPeriod` (~10 s default), so its formatting cost is
//     negligible per chunk.

import DSPAudio
import DSPBackend
import DSPConfig
import DSPLogging
import Foundation
import Synchronization

/// `@unchecked Sendable` is a *transfer* vouch, not a *share*
/// vouch: the instance is safe to cross the Thread spawn boundary
/// because exactly one thread (the loop thread) ever touches it
/// after `run()` is invoked. The rate-adjust controller, averager,
/// and stopwatch are all loop-local state with no synchronisation
/// and are *not* safe to use from multiple threads concurrently.
final class EnginePlaybackLoop: @unchecked Sendable {
  private let logger = Logger(label: "camilladsp.playback")

  private let shared: EngineSharedState
  private let capture: CaptureBackend
  private let playback: PlaybackBackend

  private let chunkSize: Int
  private let pipelineRate: Int
  private let pitchSupported: Bool

  private let rateAdjustEnabled: Bool
  private let adjustPeriod: Double
  private let targetLevel: Int

  private let onStop: (ProcessingStopReason) -> Void

  init(
    shared: EngineSharedState,
    capture: CaptureBackend,
    playback: PlaybackBackend,
    pipelineRate: Int,
    chunkSize: Int,
    rateAdjustEnabled: Bool,
    adjustPeriod: Double,
    targetLevel: Int,
    onStop: @escaping (ProcessingStopReason) -> Void
  ) {
    self.shared = shared
    self.capture = capture
    self.playback = playback
    self.pipelineRate = pipelineRate
    self.chunkSize = chunkSize
    self.pitchSupported = capture.pitchControlSupported
    self.rateAdjustEnabled = rateAdjustEnabled
    self.adjustPeriod = adjustPeriod
    self.targetLevel = targetLevel
    self.onStop = onStop
  }

  func run() {
    logger.info("Playback thread started")
    setRealtimeThreadPriority(bufferFrames: chunkSize, sampleRate: pipelineRate)
    logRateAdjustMode()

    // Rate-adjust state lives entirely on this thread.
    var rateController: PIRateController? = nil
    var averager = Averager()
    var stopwatch = Stopwatch()
    var lastSpeed: Double = 1.0
    if rateAdjustEnabled {
      rateController = PIRateController(
        samplerate: pipelineRate,
        interval: adjustPeriod,
        targetLevel: targetLevel
      )
      stopwatch.restart()
    }

    while !shared.shouldStop.load(ordering: .acquiring) {
      shared.processedSemaphore.wait()
      if shared.shouldStop.load(ordering: .acquiring) { break }

      while let chunk = shared.processedQueue.dequeue() {
        if shared.shouldStop.load(ordering: .acquiring) { return }

        // Sample the buffer fill *before* writing — measures what
        // the rate-adjust controller cares about: how much
        // already-produced audio is queued in front of the device.
        if rateAdjustEnabled, let controller = rateController {
          let ringFill = playback.bufferLevel
          let queuedFrames = shared.processedQueue.count * chunkSize
          averager.add(Double(ringFill + queuedFrames))

          if stopwatch.elapsedSeconds >= adjustPeriod, let avg = averager.average {
            let speed = controller.next(level: avg)
            stopwatch.restart()
            averager.restart()
            applySpeed(speed, lastSpeed: &lastSpeed, average: avg)
          }
        }

        do {
          try playback.write(chunk: chunk)
        } catch {
          logger.error("Playback error: %s", .string("\(error)"))
          onStop(.playbackError("\(error)"))
          return
        }
      }
    }
    logger.info("Playback thread stopped")
  }

  /// Apply a rate-adjust output. Skip if the speed change is
  /// negligible (< 1 ppm) so we don't churn the resampler ratio
  /// pointlessly.
  private func applySpeed(_ speed: Double, lastSpeed: inout Double, average: Double) {
    let changed = abs(speed - lastSpeed) > 0.000_001
    if changed {
      lastSpeed = speed
      if pitchSupported {
        capture.setPitch(speed)
      } else {
        shared.resamplerRatio.value = speed
      }
      let methodStr: StaticString = pitchSupported ? "pitch" : "resampler"
      logger.info(
        "Rate adjust: buffer=%f target=%d speed=%f via %s", .double(average),
        .int(targetLevel), .double(speed), .staticString(methodStr))
    } else {
      logger.debug(
        "Rate adjust: buffer=%f, keeping speed=%f", .double(average), .double(lastSpeed))
    }
  }

  private func logRateAdjustMode() {
    if rateAdjustEnabled {
      let methodStr: StaticString = pitchSupported ? "capture clock pitch" : "resampler ratio"
      logger.info(
        "Rate adjustment enabled (period=%fs, target_level=%d, method=%s)",
        .double(adjustPeriod), .int(targetLevel), .staticString(methodStr))
    } else {
      logger.info("Rate adjustment disabled (enable_rate_adjust not set in config)")
    }
  }
}
