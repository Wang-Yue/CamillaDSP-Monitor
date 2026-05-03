// Drift-compensation primitives used by the rate-adjust loop. Direct
// ports of the upstream pieces in `camilladsp/src/utils/`:
//
//   * `PIRateController` — PI controller (kp + ki) with a 20-step
//     ramp-back when the buffer drifts too far. Output is clamped to
//     ±0.5 % so a single tick can never make a sudden audible step.
//   * `Averager` — sum / count over the adjust period, fed once per
//     processed chunk so the controller sees a smooth long-term mean
//     rather than a single periodic snapshot.
//   * `Stopwatch` — simple monotonic timer used to detect when one
//     adjust period has elapsed; built on `clock_gettime_nsec_np`
//     (vDSO read, no syscall) to keep the hot path fast.
//
// The controller does not assume an audio backend; it only computes
// a target speed multiplier from a measured buffer level. The
// `DSPEngineCore` glue feeds it level samples and routes its output
// either to a BlackHole-style clock-pitch tweak or to the resampler
// ratio (matching upstream's "preferred / fallback" priority order).

import Foundation

// MARK: - PI rate controller

/// Proportional-integral controller that nudges the resample ratio (or
/// capture-clock pitch) so the playback buffer level stays near
/// `targetLevel`. Direct port of `PIRateController` in
/// `camilladsp/src/utils/rate_controller.rs`.
///
/// The controller's output is the *speed* multiplier — a value close
/// to `1.0` (typically `1.0 ± 0.001`). A speed `> 1.0` means "capture
/// is running too slow, push it slightly faster"; `< 1.0` is the
/// opposite. The output is clamped to `1.0 ± 0.005`.
public final class PIRateController {
  private let targetLevel: Double
  private let interval: Double
  private let kP: Double
  private let kI: Double
  private let framesPerInterval: Double
  private var accumulated: Double = 0.0
  private let rampSteps: Int
  private let rampTriggerLimit: Double
  private var rampStart: Double
  private var rampStep: Int = 0

  /// Default upstream gains. `kp = 0.2`, `ki = 0.004`, ramp over
  /// 20 steps when error exceeds 33 % of target.
  public convenience init(samplerate: Int, interval: Double, targetLevel: Int) {
    self.init(
      samplerate: samplerate,
      interval: interval,
      targetLevel: targetLevel,
      kP: 0.2,
      kI: 0.004,
      rampSteps: 20,
      rampTriggerLimit: 0.33
    )
  }

  public init(
    samplerate: Int,
    interval: Double,
    targetLevel: Int,
    kP: Double,
    kI: Double,
    rampSteps: Int,
    rampTriggerLimit: Double
  ) {
    self.targetLevel = Double(targetLevel)
    self.interval = interval
    self.kP = kP
    self.kI = kI
    self.framesPerInterval = interval * Double(samplerate)
    self.rampSteps = rampSteps
    self.rampTriggerLimit = rampTriggerLimit
    self.rampStart = Double(targetLevel)
  }

  /// Produce the next speed multiplier given the current measured
  /// buffer level. Caller should only invoke this once per
  /// `interval` seconds — the integrator term assumes a fixed
  /// sample period.
  public func next(level: Double) -> Double {
    // If we're past the ramp and the buffer has wandered more than
    // `rampTriggerLimit` away from target, restart a smooth ramp
    // back instead of letting the integrator drag us back hard.
    if rampStep >= rampSteps,
      abs((targetLevel - level) / targetLevel) > rampTriggerLimit
    {
      rampStart = level
      rampStep = 0
    }
    if rampStep == 0 {
      rampStart = level
    }

    let currentTarget: Double
    if rampStep < rampSteps {
      rampStep += 1
      // Easing: `1 - ((rampSteps - step) / rampSteps)^4`.
      let remainingFrac = Double(rampSteps - rampStep) / Double(rampSteps)
      let ease = 1.0 - pow(remainingFrac, 4)
      currentTarget = rampStart + (targetLevel - rampStart) * ease
    } else {
      currentTarget = targetLevel
    }

    let err = level - currentTarget
    let relErr = err / framesPerInterval
    accumulated += relErr * interval
    let proportional = kP * relErr
    let integral = kI * accumulated
    var output = proportional + integral
    // Cap a single-tick correction to ±0.5 % so the controller
    // can't introduce an audible step. Same clamp as upstream.
    output = Swift.max(-0.005, Swift.min(0.005, output))
    return 1.0 - output
  }

  /// Reset integrator and ramp state. Call after re-prefilling the
  /// playback buffer so the controller doesn't carry stale error
  /// across a buffer-level discontinuity.
  public func reset() {
    accumulated = 0.0
    rampStep = 0
    rampStart = targetLevel
  }
}

// MARK: - Averager

/// Sum-and-count accumulator. Producer adds one buffer-level sample
/// per chunk; the rate-adjust tick reads the average over the period
/// and restarts. Direct port of `Averager` in
/// `camilladsp/src/utils/countertimer.rs`.
public struct Averager {
  private var sum: Double = 0.0
  private var count: Int = 0

  public init() {}

  public mutating func add(_ value: Double) {
    sum += value
    count += 1
  }

  public mutating func restart() {
    sum = 0.0
    count = 0
  }

  public var average: Double? {
    count > 0 ? sum / Double(count) : nil
  }
}

// MARK: - Stopwatch

/// Monotonic, allocation-free elapsed-time helper. `clock_gettime_nsec_np`
/// is a vDSO read on Darwin — no syscall — so calling it once per
/// processed chunk has negligible overhead.
public struct Stopwatch {
  private var startNs: UInt64

  public init() { startNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) }

  public mutating func restart() { startNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) }

  public var elapsedSeconds: Double {
    let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
    return Double(now - startNs) / 1_000_000_000.0
  }
}
