// Tests for the PI rate-adjust controller and the supporting
// `Averager` / `Stopwatch` helpers in `Engine/RateController.swift`.

import XCTest

@testable import CamillaDSPLib

final class RateControllerTests: XCTestCase {

  // MARK: - PIRateController

  func testReturnsUnitySpeedAtTarget() {
    // When the measured level matches the target exactly the
    // controller should return 1.0 — no correction.
    let pi = PIRateController(samplerate: 48000, interval: 1.0, targetLevel: 1024)
    // The first call always seeds the ramp at the measured level
    // (effectively making `current_target == level`), so output
    // is 1.0.
    XCTAssertEqual(pi.next(level: 1024), 1.0, accuracy: 1e-12)
  }

  func testHighBufferTriggersSlowdown() {
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
    XCTAssertLessThan(lastSpeed, 1.0, "expected slowdown for over-full buffer")
    // Output is clamped to the upstream ±0.5% range.
    XCTAssertGreaterThanOrEqual(lastSpeed, 1.0 - 0.005)
  }

  func testLowBufferTriggersSpeedup() {
    let pi = PIRateController(samplerate: 48000, interval: 1.0, targetLevel: 1024)
    var lastSpeed = 1.0
    for _ in 0..<25 {
      lastSpeed = pi.next(level: 256)
    }
    XCTAssertGreaterThan(lastSpeed, 1.0, "expected speed-up for under-full buffer")
    XCTAssertLessThanOrEqual(lastSpeed, 1.0 + 0.005)
  }

  func testOutputAlwaysClampedWithin5PerMille() {
    // Even an absurdly out-of-spec level shouldn't push the
    // controller past ±0.5% — the upstream clamp guarantees
    // there's no audible step from a single tick.
    let pi = PIRateController(samplerate: 48000, interval: 1.0, targetLevel: 1024)
    for _ in 0..<200 { _ = pi.next(level: 1_000_000) }
    let speed = pi.next(level: 1_000_000)
    XCTAssertGreaterThanOrEqual(speed, 1.0 - 0.005)
    XCTAssertLessThanOrEqual(speed, 1.0 + 0.005)
  }

  // MARK: - Averager

  func testAveragerReturnsNilWhenEmpty() {
    let avg = Averager()
    XCTAssertNil(avg.average)
  }

  func testAveragerComputesMean() {
    var avg = Averager()
    avg.add(10)
    avg.add(20)
    avg.add(30)
    XCTAssertEqual(avg.average ?? .nan, 20.0, accuracy: 1e-12)
  }

  func testAveragerRestartClearsState() {
    var avg = Averager()
    avg.add(100)
    avg.restart()
    XCTAssertNil(avg.average)
    avg.add(5)
    XCTAssertEqual(avg.average, 5.0)
  }

  // MARK: - Stopwatch

  func testStopwatchElapsesMonotonically() {
    var sw = Stopwatch()
    let t0 = sw.elapsedSeconds
    Thread.sleep(forTimeInterval: 0.01)
    let t1 = sw.elapsedSeconds
    XCTAssertGreaterThan(t1, t0)
    sw.restart()
    XCTAssertLessThan(sw.elapsedSeconds, t1)
  }
}
