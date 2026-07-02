import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters

@Suite struct DiffEqTests {
  private static func isClose(_ left: PrcFmt, _ right: PrcFmt, maxdiff: PrcFmt) -> Bool {
    return abs(left - right) < maxdiff
  }

  private static func compareWaveforms(_ left: [PrcFmt], _ right: [PrcFmt], maxdiff: PrcFmt) -> Bool
  {
    guard left.count == right.count else { return false }
    for (val_l, val_r) in zip(left, right) {
      if !isClose(val_l, val_r, maxdiff: maxdiff) {
        return false
      }
    }
    return true
  }

  @Test func check_result() {
    let params = DiffEqParameters(
      a: [1.0, -0.1462978543780541, 0.005350765548905586],
      b: [0.21476322779271284, 0.4295264555854257, 0.21476322779271284]
    )
    let filter = DiffEqFilter(parameters: params)

    var wave: [PrcFmt] = [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    let expected: [PrcFmt] = [0.215, 0.461, 0.281, 0.039, 0.004, 0.0, 0.0, 0.0]

    filter.process(waveform: &wave)

    #expect(Self.compareWaveforms(wave, expected, maxdiff: 1e-3))
  }
}
