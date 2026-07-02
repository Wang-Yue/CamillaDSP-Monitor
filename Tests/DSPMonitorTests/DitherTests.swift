import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters

@Suite struct DitherTests {
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

  @Test func test_quantize() {
    var waveform: [PrcFmt] = [-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0]
    let waveform2 = waveform
    let params = DitherParameters(type: .none, bits: 8)
    let filter = DitherFilter(parameters: params)
    filter.process(waveform: &waveform)

    #expect(Self.compareWaveforms(waveform, waveform2, maxdiff: 1.0 / 128.0))
    #expect(Self.isClose((128.0 * waveform[2]).rounded(), 128.0 * waveform[2], maxdiff: 1e-9))
  }

  @Test func test_flat() {
    var waveform: [PrcFmt] = [-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0]
    let waveform2 = waveform
    let params = DitherParameters(type: .flat, bits: 8, amplitude: 2.0)
    let filter = DitherFilter(parameters: params)
    filter.process(waveform: &waveform)

    #expect(Self.compareWaveforms(waveform, waveform2, maxdiff: 1.0 / 64.0))
    #expect(Self.isClose((128.0 * waveform[2]).rounded(), 128.0 * waveform[2], maxdiff: 1e-9))
  }

  @Test func test_high_pass() {
    var waveform: [PrcFmt] = [-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0]
    let waveform2 = waveform
    let params = DitherParameters(type: .highpass, bits: 8)
    let filter = DitherFilter(parameters: params)
    filter.process(waveform: &waveform)

    #expect(Self.compareWaveforms(waveform, waveform2, maxdiff: 1.0 / 32.0))
    #expect(Self.isClose((128.0 * waveform[2]).rounded(), 128.0 * waveform[2], maxdiff: 1e-9))
  }

  @Test func test_lip() {
    var waveform: [PrcFmt] = [-1.0, -0.5, -1.0 / 3.0, 0.0, 1.0 / 3.0, 0.5, 1.0]
    let waveform2 = waveform
    let params = DitherParameters(type: .lipshitz441, bits: 8)
    let filter = DitherFilter(parameters: params)
    filter.process(waveform: &waveform)

    #expect(Self.compareWaveforms(waveform, waveform2, maxdiff: 1.0 / 16.0))
    #expect(Self.isClose((128.0 * waveform[2]).rounded(), 128.0 * waveform[2], maxdiff: 1e-9))
  }
}
