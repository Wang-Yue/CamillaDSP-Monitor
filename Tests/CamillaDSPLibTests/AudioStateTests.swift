import XCTest

@testable import CamillaDSPLib

final class AudioStateTests: XCTestCase {

  func testProcessingParametersGettersSetters() {
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)

    params.targetVolume = -10.0
    XCTAssertEqual(params.targetVolume, -10.0)

    params.currentVolume = -12.0
    XCTAssertEqual(params.currentVolume, -12.0)

    params.isMuted = true
    XCTAssertTrue(params.isMuted)

    params.processingLoad = 45.0
    XCTAssertEqual(params.processingLoad, 45.0)

    params.captureSignalPeak = [-3.0, -4.0]
    XCTAssertEqual(params.captureSignalPeak, [-3.0, -4.0])

    params.captureSignalRms = [-10.0, -11.0]
    XCTAssertEqual(params.captureSignalRms, [-10.0, -11.0])

    params.playbackSignalPeak = [-1.0, -2.0]
    XCTAssertEqual(params.playbackSignalPeak, [-1.0, -2.0])

    params.playbackSignalRms = [-8.0, -9.0]
    XCTAssertEqual(params.playbackSignalRms, [-8.0, -9.0])
  }

  func testProcessingParametersMultiChannelSetters() {
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)

    params.setCaptureSignalPeak([-5.0, -6.0])
    XCTAssertEqual(params.captureSignalPeak, [-5.0, -6.0])

    params.setCaptureSignalRms([-15.0, -16.0])
    XCTAssertEqual(params.captureSignalRms, [-15.0, -16.0])

    params.setPlaybackSignalPeak([-2.0, -3.0])
    XCTAssertEqual(params.playbackSignalPeak, [-2.0, -3.0])

    params.setPlaybackSignalRms([-12.0, -13.0])
    XCTAssertEqual(params.playbackSignalRms, [-12.0, -13.0])
  }

  func testProcessingParametersUpdateLevels() {
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    var chunk = AudioChunk(frames: 1024, channels: 2)

    // Fill with 1.0 (0dB peak, 0dB RMS)
    for ch in 0..<2 {
      for t in 0..<1024 {
        chunk.waveforms[ch][t] = 1.0
      }
    }

    let loudestCapture = params.updateCaptureLevels(from: chunk)
    XCTAssertEqual(loudestCapture, 0.0, accuracy: 1e-3)
    XCTAssertEqual(params.captureSignalPeak[0], 0.0, accuracy: 1e-3)
    XCTAssertEqual(params.captureSignalRms[0], 0.0, accuracy: 1e-3)

    let loudestPlayback = params.updatePlaybackLevels(from: chunk)
    XCTAssertEqual(loudestPlayback, 0.0, accuracy: 1e-3)
    XCTAssertEqual(params.playbackSignalPeak[0], 0.0, accuracy: 1e-3)
    XCTAssertEqual(params.playbackSignalRms[0], 0.0, accuracy: 1e-3)
  }

  func testDSPOpsScalarMultiply() {
    var buffer: [PrcFmt] = [1.0, 2.0, 3.0]
    DSPOps.scalarMultiply(&buffer, by: 2.0)
    XCTAssertEqual(buffer, [2.0, 4.0, 6.0])
  }

  func testDSPOpsAdd() {
    let a: [PrcFmt] = [1.0, 2.0, 3.0]
    var b: [PrcFmt] = [4.0, 5.0, 6.0]
    DSPOps.add(a, &b, count: 2)  // Only add first 2 elements!
    XCTAssertEqual(b, [5.0, 7.0, 6.0])
  }

  func testDSPOpsMultiply() {
    let a: [PrcFmt] = [1.0, 2.0, 3.0]
    let b: [PrcFmt] = [4.0, 5.0, 6.0]
    var result = [PrcFmt](repeating: 0.0, count: 3)
    DSPOps.multiply(a, b, result: &result, count: 2)
    XCTAssertEqual(result, [4.0, 10.0, 0.0])
  }

  func testDSPOpsMultiplyAdd() {
    let a: [PrcFmt] = [1.0, 2.0, 3.0]
    var acc: [PrcFmt] = [4.0, 5.0, 6.0]
    DSPOps.multiplyAdd(a, 2.0, accumulator: &acc, count: 2)  // acc = (a * 2) + acc
    XCTAssertEqual(acc, [6.0, 9.0, 6.0])
  }

  func testDSPOpsPeakAndRMS() {
    let buffer: [PrcFmt] = [1.0, -2.0, 3.0]
    XCTAssertEqual(DSPOps.peakAbsolute(buffer), 3.0)
    // RMS of [1, -2, 3] = sqrt((1 + 4 + 9) / 3) = sqrt(14/3) = 2.16024...
    XCTAssertEqual(DSPOps.rms(buffer), sqrt(14.0 / 3.0), accuracy: 1e-5)
  }
}
