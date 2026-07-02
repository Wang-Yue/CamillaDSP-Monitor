import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters

@Suite struct DelayTests {
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

  @Test func delay_small() {
    var waveform: [PrcFmt] = [0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    let waveform_delayed: [PrcFmt] = [0.0, 0.0, 0.0, 0.0, -0.5, 1.0, 0.0, 0.0]
    let params = DelayParameters(delay: 3.0, unit: .samples, subsample: false)
    let filter = DelayFilter(parameters: params, sampleRate: 44100)
    filter.process(waveform: &waveform)
    #expect(waveform == waveform_delayed)
  }

  @Test func delay_supersmall() {
    var waveform: [PrcFmt] = [0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    let waveform_delayed = waveform
    let params = DelayParameters(delay: 0.1, unit: .samples, subsample: false)
    let filter = DelayFilter(parameters: params, sampleRate: 44100)
    filter.process(waveform: &waveform)
    #expect(waveform == waveform_delayed)
  }

  @Test func delay_large() {
    var waveform1: [PrcFmt] = [0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    var waveform2 = [PrcFmt](repeating: 0.0, count: 8)
    let waveform_delayed: [PrcFmt] = [0.0, 0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0]
    let params = DelayParameters(delay: 9.0, unit: .samples, subsample: false)
    let filter = DelayFilter(parameters: params, sampleRate: 44100)
    filter.process(waveform: &waveform1)
    filter.process(waveform: &waveform2)
    #expect(waveform1 == [PrcFmt](repeating: 0.0, count: 8))
    #expect(waveform2 == waveform_delayed)
  }

  @Test func delay_fraction() {
    var waveform: [PrcFmt] = [0.0, -0.5, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    let expected_waveform: [PrcFmt] = [
      0.0,
      0.01051051051051051,
      -0.13446780113446782,
      -0.2476751025299573,
      1.0522122611990257,
      -0.23903133046978262,
      0.07523664949897024,
      -0.021743938066703532,
      0.006413537427714274,
      -0.001882310318672015,
    ]
    let params = DelayParameters(delay: 1.7, unit: .samples, subsample: true)
    let filter = DelayFilter(parameters: params, sampleRate: 44100)
    filter.process(waveform: &waveform)
    #expect(Self.compareWaveforms(waveform, expected_waveform, maxdiff: 1.0e-6))
  }
}
