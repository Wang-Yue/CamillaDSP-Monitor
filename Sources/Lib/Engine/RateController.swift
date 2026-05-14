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
  private let setpoint: Double
  private let samplePeriod: Double
  private let kp: Double
  private let ki: Double
  private let maxAdjustment: Double

  /// Integrator state. Bounded to `±maxAdjustment` after every update
  /// so that the integrator alone cannot push the unsaturated control
  /// signal past the limit.
  private var integrator: Double = 0.0

  /// Convenience initializer with a tuning chosen for typical audio
  /// rate-adjust use: `ωn = 0.1 rad/s` (≈ 10 s closed-loop response)
  /// and `ζ = √2/2` (no overshoot in practice). Output is bounded to
  /// ±2000 ppm — the de-facto industry target for inaudible
  /// asynchronous sample-rate corrections.
  internal convenience init(samplerate: Int, interval: Double, targetLevel: Int) {
    let omegaN = 0.1
    let zeta = 0.7071067811865476  // √2/2
    let fs = Double(samplerate)
    self.init(
      samplerate: samplerate,
      interval: interval,
      targetLevel: targetLevel,
      kp: 2.0 * zeta * omegaN / fs,
      ki: (omegaN * omegaN) / fs,
      maxAdjustment: 0.002
    )
  }

  /// Designated initializer. `samplerate` is unused by the algorithm
  /// itself — gains are passed in directly — but kept in the
  /// signature so the public API matches the convenience form and so
  /// callers that prefer to specify `Fs`-relative gains have a single
  /// place to do it.
  internal init(
    samplerate: Int,
    interval: Double,
    targetLevel: Int,
    kp: Double,
    ki: Double,
    maxAdjustment: Double
  ) {
    _ = samplerate  // gains are absolute; samplerate retained for API symmetry
    self.setpoint = Double(targetLevel)
    self.samplePeriod = interval
    self.kp = kp
    self.ki = ki
    self.maxAdjustment = abs(maxAdjustment)
  }

  /// Advance the controller by one sample period and return the next
  /// speed multiplier. The caller is expected to invoke this exactly
  /// once per `interval` seconds; the integrator term assumes a fixed
  /// step.
  internal func next(level: Double) -> Double {
    let error = setpoint - level

    // Forward (rectangular) Euler integration of the error. Single
    // sample, single multiply — no allocation, no branching beyond
    // the clamp below.
    integrator += ki * samplePeriod * error

    // Conditional integrator clamping (anti-windup). Bounding the
    // integrator state itself, rather than a separate
    // back-calculation term, keeps the implementation
    // single-parameter and produces the same steady-state behavior:
    // once the controller saturates, the integrator stops growing.
    if integrator > maxAdjustment {
      integrator = maxAdjustment
    } else if integrator < -maxAdjustment {
      integrator = -maxAdjustment
    }

    let unsaturated = kp * error + integrator
    let saturated = Swift.max(-maxAdjustment, Swift.min(maxAdjustment, unsaturated))
    return 1.0 + saturated
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
