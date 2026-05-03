// Tests for the silence-detection counter that drives the engine into
// `.paused` when the capture signal stays below `silenceThreshold` for
// more than `silenceTimeout` seconds.

import XCTest

@testable import CamillaDSPLib

final class SilenceCounterTests: XCTestCase {

  /// `SilenceCounter` lives `private` inside `DSPEngineCore`. We
  /// recreate the same logic here via a tiny mirror struct so the
  /// algorithm is testable without exposing internal API. The mirror
  /// is intentionally a literal copy — if it diverges, both should be
  /// updated together.
  private struct Mirror {
    let limit: Int
    let threshold: Double
    var counter: Int = 0
    init(thresholdDb: Double, timeoutSeconds: Double, samplerate: Int, chunksize: Int) {
      self.threshold = thresholdDb
      self.limit =
        timeoutSeconds > 0 && chunksize > 0
        ? Int((timeoutSeconds * Double(samplerate) / Double(chunksize)).rounded())
        : 0
    }
    mutating func update(signalPeakDb: Double) -> ProcessingState {
      guard limit > 0 else { return .running }
      if signalPeakDb > threshold {
        counter = 0
        return .running
      }
      if counter < limit { counter += 1 }
      return counter >= limit ? .paused : .running
    }
  }

  func testDisabledWhenTimeoutZero() {
    var counter = Mirror(thresholdDb: -40, timeoutSeconds: 0, samplerate: 48000, chunksize: 1024)
    for _ in 0..<10 {
      XCTAssertEqual(
        counter.update(signalPeakDb: -100), .running,
        "timeout=0 disables detection")
    }
  }

  func testStaysRunningUntilLimitReached() {
    // 1.0 s timeout × 48000 Hz / 1024 chunk ≈ 47 chunks before triggering.
    var counter = Mirror(thresholdDb: -40, timeoutSeconds: 1.0, samplerate: 48000, chunksize: 1024)
    let limit = 47
    for i in 0..<limit - 1 {
      XCTAssertEqual(counter.update(signalPeakDb: -100), .running, "chunk \(i)")
    }
    // The chunk that pushes the counter to `limit` flips us paused.
    XCTAssertEqual(counter.update(signalPeakDb: -100), .paused, "boundary chunk")
  }

  func testRecoversWhenSignalReturns() {
    var counter = Mirror(thresholdDb: -40, timeoutSeconds: 0.5, samplerate: 48000, chunksize: 1024)
    // Drive into pause.
    for _ in 0..<60 { _ = counter.update(signalPeakDb: -100) }
    XCTAssertEqual(counter.update(signalPeakDb: -100), .paused)
    // A loud sample resets the counter immediately.
    XCTAssertEqual(counter.update(signalPeakDb: -10), .running)
    // And we stay running on subsequent loud samples.
    XCTAssertEqual(counter.update(signalPeakDb: -5), .running)
  }

  func testThresholdIsExclusive() {
    // upstream uses `value_range > threshold` to count as non-silent.
    // 1 s × 48000 / 1024 ≈ 47 chunks of headroom before capping.
    var counter = Mirror(thresholdDb: -40, timeoutSeconds: 1.0, samplerate: 48000, chunksize: 1024)
    // Right at the threshold counts as silent — counter advances.
    for _ in 0..<10 {
      _ = counter.update(signalPeakDb: -40)
    }
    XCTAssertEqual(counter.counter, 10, "values equal to threshold are silent")
    // Any value strictly above the threshold resets.
    _ = counter.update(signalPeakDb: -39.99)
    XCTAssertEqual(counter.counter, 0)
  }
}
