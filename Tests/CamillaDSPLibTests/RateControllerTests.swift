// Tests for the PI rate-adjust controller and the supporting
// `Averager` / `Stopwatch` helpers in `Engine/RateController.swift`.

import Foundation
import Testing

@testable import DSPAudio
@testable import DSPEngine

@Suite struct RateControllerTests {

  // MARK: - PIRateController

  @Test func ReturnsUnitySpeedAtTarget() {
    // When the measured level matches the target exactly the
    // controller should return 1.0 — no correction.
    let pi = PIRateController(samplerate: 48000, interval: 1.0, targetLevel: 1024)
    // The first call always seeds the ramp at the measured level
    // (effectively making `current_target == level`), so output
    // is 1.0.
    #expect(abs(pi.next(level: 1024) - 1.0) <= 1e-12)
  }

  @Test func HighBufferTriggersSlowdown() {
    // Level above target → controller should ask capture to
    // slow down → output speed < 1.0.
    let pi = PIRateController(samplerate: 48000, interval: 1.0, targetLevel: 1024)
    // First call seeds ramp; subsequent calls during the ramp
    // ease the target up. By the time the ramp completes
    // (rampSteps = 20), the controller compares against the
    // real target.
    var lastSpeed = 1.0
    for _ in 0..<25 {
      lastSpeed = pi.next(level: 4096)
    }
    #expect(lastSpeed < 1.0)
    // Output is clamped to the upstream ±0.5% range.
    #expect(lastSpeed >= 1.0 - 0.005)
  }

  @Test func LowBufferTriggersSpeedup() {
    let pi = PIRateController(samplerate: 48000, interval: 1.0, targetLevel: 1024)
    var lastSpeed = 1.0
    for _ in 0..<25 {
      lastSpeed = pi.next(level: 256)
    }
    #expect(lastSpeed > 1.0)
    #expect(lastSpeed <= 1.0 + 0.005)
  }

  @Test func OutputAlwaysClampedWithin5PerMille() {
    // Even an absurdly out-of-spec level shouldn't push the
    // controller past ±0.5% — the upstream clamp guarantees
    // there's no audible step from a single tick.
    let pi = PIRateController(samplerate: 48000, interval: 1.0, targetLevel: 1024)
    for _ in 0..<200 { _ = pi.next(level: 1_000_000) }
    let speed = pi.next(level: 1_000_000)
    #expect(speed >= 1.0 - 0.005)
    #expect(speed <= 1.0 + 0.005)
  }

  // MARK: - Averager

  @Test func AveragerReturnsNilWhenEmpty() {
    let avg = Averager()
    #expect(avg.average == nil)
  }

  @Test func AveragerComputesMean() {
    var avg = Averager()
    avg.add(10)
    avg.add(20)
    avg.add(30)
    #expect(abs((avg.average ?? .nan) - 20.0) <= 1e-12)
  }

  @Test func AveragerRestartClearsState() {
    var avg = Averager()
    avg.add(100)
    avg.restart()
    #expect(avg.average == nil)
    avg.add(5)
    #expect(avg.average == 5.0)
  }

  // MARK: - Stopwatch

  @Test func StopwatchElapsesMonotonically() {
    var sw = Stopwatch()
    let t0 = sw.elapsedSeconds
    Thread.sleep(forTimeInterval: 0.01)
    let t1 = sw.elapsedSeconds
    #expect(t1 > t0)
    sw.restart()
    #expect(sw.elapsedSeconds < t1)
  }
}
