import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters

@Suite struct LimiterTests {
  private static func isClose(_ left: PrcFmt, _ right: PrcFmt, maxdiff: PrcFmt) -> Bool {
    return abs(left - right) < maxdiff
  }

  @Test func test_hard_clip() {
    var waveform: [PrcFmt] = [-2.0, -1.0, 0.0, 0.5, 1.5, 2.0]
    let params = LimiterParameters(clipLimit: -6.020599913279624, softClip: false)  // -6.02 dB = 0.5 linear limit
    let filter = LimiterFilter(parameters: params)
    filter.process(waveform: &waveform)

    let expected: [PrcFmt] = [-0.5, -0.5, 0.0, 0.5, 0.5, 0.5]
    for (got, exp) in zip(waveform, expected) {
      #expect(Self.isClose(got, exp, maxdiff: 1e-5))
    }
  }

  @Test func test_soft_clip() {
    var waveform: [PrcFmt] = [-2.0, -0.5, 0.0, 0.5, 2.0]
    let params = LimiterParameters(clipLimit: 0.0, softClip: true)  // 0 dB = 1.0 linear limit
    let filter = LimiterFilter(parameters: params)
    filter.process(waveform: &waveform)

    // soft clip formula: scaled = clamp(scaled, -1.5, 1.5); out = (scaled - scaled^3/6.75) * limit
    // For input 2.0: scaled = 2.0 -> clamped to 1.5. out = (1.5 - 1.5^3/6.75) = 1.5 - 3.375/6.75 = 1.5 - 0.5 = 1.0
    // For input 0.5: scaled = 0.5. out = 0.5 - 0.125/6.75 = 0.5 - 0.0185185 = 0.481481
    let expected: [PrcFmt] = [-1.0, -0.481481, 0.0, 0.481481, 1.0]
    for (got, exp) in zip(waveform, expected) {
      #expect(Self.isClose(got, exp, maxdiff: 1e-5))
    }
  }
}
