// Tests for the silence-detection counter that drives the engine into
// `.paused` when the capture signal stays below `silenceThreshold` for
// more than `silenceTimeout` seconds.

import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig

@Suite struct SilenceCounterTests {

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

  @Test func DisabledWhenTimeoutZero() {
    var counter = Mirror(thresholdDb: -40, timeoutSeconds: 0, samplerate: 48000, chunksize: 1024)
    for _ in 0..<10 {
      #expect(counter.update(signalPeakDb: -100) == .running)
    }
  }

  @Test func StaysRunningUntilLimitReached() {
    // 1.0 s timeout × 48000 Hz / 1024 chunk ≈ 47 chunks before triggering.
    var counter = Mirror(thresholdDb: -40, timeoutSeconds: 1.0, samplerate: 48000, chunksize: 1024)
    let limit = 47
    for _ in 0..<limit - 1 {
      #expect(counter.update(signalPeakDb: -100) == .running)
    }
    // The chunk that pushes the counter to `limit` flips us paused.
    #expect(counter.update(signalPeakDb: -100) == .paused)
  }

  @Test func RecoversWhenSignalReturns() {
    var counter = Mirror(thresholdDb: -40, timeoutSeconds: 0.5, samplerate: 48000, chunksize: 1024)
    // Drive into pause.
    for _ in 0..<60 { _ = counter.update(signalPeakDb: -100) }
    #expect(counter.update(signalPeakDb: -100) == .paused)
    // A loud sample resets the counter immediately.
    #expect(counter.update(signalPeakDb: -10) == .running)
    // And we stay running on subsequent loud samples.
    #expect(counter.update(signalPeakDb: -5) == .running)
  }

  @Test func ThresholdIsExclusive() {
    // upstream uses `value_range > threshold` to count as non-silent.
    // 1 s × 48000 / 1024 ≈ 47 chunks of headroom before capping.
    var counter = Mirror(thresholdDb: -40, timeoutSeconds: 1.0, samplerate: 48000, chunksize: 1024)
    // Right at the threshold counts as silent — counter advances.
    for _ in 0..<10 {
      _ = counter.update(signalPeakDb: -40)
    }
    #expect(counter.counter == 10)
    // Any value strictly above the threshold resets.
    _ = counter.update(signalPeakDb: -39.99)
    #expect(counter.counter == 0)
  }
}
