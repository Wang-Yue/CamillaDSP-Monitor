import Foundation
import Testing

@testable import DSPAudio

@Suite struct AudioStateTests {

  @Test func ProcessingParametersGettersSetters() {
    let params = ProcessingParameters()

    params.targetVolume = -10.0
    #expect(params.targetVolume == -10.0)

    params.currentVolume = -12.0
    #expect(params.currentVolume == -12.0)

    params.isMuted = true
    #expect(params.isMuted)
  }

  @Test func ProcessingParametersUpdateLevels() {
    let params = ProcessingParameters()
    let chunk = AudioChunk(frames: 1024, channels: 2)

    // Fill with 1.0 (0dB peak)
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk[ch][t] = 1.0
      }
    }

    let loudestCapture = params.updateCaptureLevels(from: chunk)
    #expect(abs(loudestCapture - 0.0) <= 1e-3)
  }

  @Test func DSPOpsScalarMultiply() {
    var buffer: [PrcFmt] = [1.0, 2.0, 3.0]
    DSPOps.scalarMultiply(&buffer, by: 2.0)
    #expect(buffer == [2.0, 4.0, 6.0])
  }

  @Test func DSPOpsAdd() {
    let a: [PrcFmt] = [1.0, 2.0, 3.0]
    var b: [PrcFmt] = [4.0, 5.0, 6.0]
    DSPOps.add(a, &b, count: 2)  // Only add first 2 elements!
    #expect(b == [5.0, 7.0, 6.0])
  }

  @Test func DSPOpsMultiply() {
    let a: [PrcFmt] = [1.0, 2.0, 3.0]
    let b: [PrcFmt] = [4.0, 5.0, 6.0]
    var result = [PrcFmt](repeating: 0.0, count: 3)
    DSPOps.multiply(a, b, result: &result, count: 2)
    #expect(result == [4.0, 10.0, 0.0])
  }

  @Test func DSPOpsMultiplyAdd() {
    let a: [PrcFmt] = [1.0, 2.0, 3.0]
    var acc: [PrcFmt] = [4.0, 5.0, 6.0]
    DSPOps.multiplyAdd(a, 2.0, accumulator: &acc, count: 2)  // acc = (a * 2) + acc
    #expect(acc == [6.0, 9.0, 6.0])
  }

  @Test func DSPOpsPeakAndRMS() {
    let buffer: [PrcFmt] = [1.0, -2.0, 3.0]
    #expect(DSPOps.peakAbsolute(buffer) == 3.0)
    // RMS of [1, -2, 3] = sqrt((1 + 4 + 9) / 3) = sqrt(14/3) = 2.16024...
    #expect(abs(DSPOps.rms(buffer) - sqrt(14.0 / 3.0)) <= 1e-5)
  }
}
