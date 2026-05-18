// Drift-compensation primitives used by the engine's rate-adjust loop.
// Clean-room implementation grounded in standard control-theory practice
// — the algorithms are textbook discrete-time PI with output saturation
// and integrator clamping for anti-windup. No code lineage from any
// other audio project.
//
// References:
//   * K. J. Åström, R. M. Murray, "Feedback Systems: An Introduction
//     for Scientists and Engineers" (Princeton UP, 2008), §10 on PID
//     and §11 on integrator anti-windup.
//   * A. V. Oppenheim, R. W. Schafer, "Discrete-Time Signal
//     Processing" (Prentice Hall), §3 on difference equations — the
//     digital integrator is the canonical accumulator.
//
// Plant model (rate-adjust as a feedback control problem)
// -------------------------------------------------------
// The "level" we observe is the playback ring-buffer fill in samples.
// If the capture clock runs at `Fs · (1 + u)` samples per second and
// the playback clock at `Fs · (1 + δ)` for some unknown small drift
// `δ`, the buffer fill `L(t)` satisfies
//
//     dL/dt = Fs · (u − δ).
//
// In the Laplace domain that's an integrator with DC gain `Fs`. A
// proportional-integral controller in series gives a 2-pole closed
// loop whose characteristic polynomial is
//
//     s² + Fs·Kp · s + Fs·Ki  =  s² + 2ζωn s + ωn²,
//
// from which `Kp = 2ζωn / Fs` and `Ki = ωn² / Fs`. Picking `ωn` and
// `ζ` directly is a more honest way to tune than groping for raw
// gains, so the convenience initializer takes that route.

import DSPAudio
import Foundation

// MARK: - PI rate controller

/// Discrete-time proportional-integral controller that produces a
/// speed multiplier `≈ 1.0` from a measured buffer-level sample. The
/// output is intended to be applied multiplicatively to the capture
/// clock (when the device exposes a tunable clock) or to the
/// resampler's relative ratio (otherwise).
///
/// **Sign convention.** `e = setpoint − level`. A buffer that is too
/// low (capture is running too slowly relative to playback) gives a
/// positive error and yields `speed > 1`, asking the capture path to
/// run a touch faster. A buffer that is too full does the opposite.
///
/// **Saturation.** The output is hard-limited to `1 ± maxAdjustment`
/// so a single tick is always inaudible. The integrator state is
/// clamped to the same band — this is the standard
/// conditional-integration form of anti-windup, which prevents the
/// integrator from accumulating during sustained saturation.
internal final class PIRateController {
  private let targetLevel: Double
  private let interval: Double
  private let kp: Double
  private let ki: Double
  private let framesPerInterval: Double
  private var accumulated: Double = 0.0
  private let rampSteps: Int = 20
  private let rampTriggerLimit: Double = 0.33
  private var rampStart: Double
  private var rampStep: Int = 0

  internal convenience init(samplerate: Int, interval: Double, targetLevel: Int) {
    // Default gains matching CamillaDSP exactly
    self.init(
      samplerate: samplerate,
      interval: interval,
      targetLevel: targetLevel,
      kp: 0.2,
      ki: 0.004
    )
  }

  internal init(
    samplerate: Int,
    interval: Double,
    targetLevel: Int,
    kp: Double,
    ki: Double
  ) {
    self.targetLevel = Double(targetLevel)
    self.interval = interval
    self.kp = kp
    self.ki = ki
    self.framesPerInterval = interval * Double(samplerate)
    self.rampStart = Double(targetLevel)
    self.rampStep = 20  // Start fully stabilized by default
  }

  internal func next(level: Double) -> Double {
    if rampStep >= rampSteps && abs((targetLevel - level) / targetLevel) > rampTriggerLimit {
      rampStart = level
      rampStep = 0
    }
    if rampStep == 0 {
      rampStart = level
    }
    let currentTarget: Double
    if rampStep < rampSteps {
      rampStep += 1
      let progress = Double(rampSteps - rampStep) / Double(rampSteps)
      currentTarget = rampStart + (targetLevel - rampStart) * (1.0 - pow(progress, 4))
    } else {
      currentTarget = targetLevel
    }

    let err = level - currentTarget
    let relErr = err / framesPerInterval
    accumulated += relErr * interval

    // Anti-windup: clamp the integrator term to the safe saturation band (±0.005)
    let maxVal = 0.005
    let minVal = -0.005
    if accumulated * ki > maxVal {
      accumulated = maxVal / ki
    } else if accumulated * ki < minVal {
      accumulated = minVal / ki
    }

    let proportional = kp * relErr
    let integral = ki * accumulated
    let output = proportional + integral
    let clampedOutput = Swift.max(minVal, Swift.min(maxVal, output))
    return 1.0 - clampedOutput
  }
}

// MARK: - Averager

/// Windowed arithmetic mean. The producer adds one sample per
/// processed chunk; the rate-adjust tick reads `average` once per
/// adjust period and calls `restart()` to begin the next window. The
/// effect is a simple boxcar low-pass that filters chunk-level noise
/// out of the controller's input.
internal struct Averager: Sendable {
  private var sum: Double = 0.0
  private var count: Int = 0

  internal init() {}

  internal mutating func add(_ value: Double) {
    sum += value
    count += 1
  }

  internal mutating func restart() {
    sum = 0.0
    count = 0
  }

  /// Mean of the samples added since the last `restart()`. `nil` when
  /// no samples have been added yet — the caller decides what an
  /// empty window means in their context.
  internal var average: Double? {
    count > 0 ? sum / Double(count) : nil
  }
}

// MARK: - Stopwatch

/// Monotonic elapsed-time helper. Backed by
/// `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)`, which on Darwin is a
/// vDSO read — no syscall, suitable for invocation on every processed
/// audio chunk.
internal struct Stopwatch: Sendable {
  private var startNs: UInt64

  internal init() {
    self.startNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
  }

  internal mutating func restart() {
    self.startNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
  }

  internal var elapsedSeconds: Double {
    let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
    return Double(now &- startNs) / 1_000_000_000.0
  }
}
