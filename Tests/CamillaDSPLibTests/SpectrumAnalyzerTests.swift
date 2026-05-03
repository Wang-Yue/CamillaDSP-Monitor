// Sanity tests for the FFT spectrum analyzer. The Rust upstream tests
// (camilladsp/src/spectrum.rs) check that a 0 dBFS sine peaks near 0 dBFS;
// we replicate that and exercise the log-bin geometry and ring-buffer
// channel-out-of-range path.

import XCTest

@testable import CamillaDSPLib

final class SpectrumAnalyzerTests: XCTestCase {

  /// Build a stereo `AudioChunk` of length `frames` carrying a unit-amplitude
  /// sine wave at `freq` on every channel.
  private func sineChunk(freq: Double, samplerate: Int, frames: Int, channels: Int = 2)
    -> AudioChunk
  {
    let dt = 2.0 * .pi * freq / Double(samplerate)
    let waveform = (0..<frames).map { sin(dt * Double($0)) }
    let waveforms = Array(repeating: waveform, count: channels)
    return AudioChunk(waveforms: waveforms)
  }

  func testFftLengthForRoundsUpToPowerOfTwo() {
    // 44100 / 20 ≈ 2205, next power of two is 4096.
    XCTAssertEqual(fftLengthFor(minFreq: 20, samplerate: 44100), 4096)
    // 48000 / 1000 = 48, next power of two is 64.
    XCTAssertEqual(fftLengthFor(minFreq: 1000, samplerate: 48000), 64)
  }

  func testSineProducesPeakAtCarrier() throws {
    let analyzer = SpectrumAnalyzer()
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 2)
    let samplerate = 48000

    // Push enough chunks to cover the entire 8192-sample FFT window.
    for _ in 0..<16 {
      buffer.append(chunk: sineChunk(freq: 1000, samplerate: samplerate, frames: 1024))
    }

    let result = try analyzer.compute(
      buffer: buffer,
      channel: 0,
      minFreq: 20,
      maxFreq: 20000,
      nBins: 64,
      samplerate: samplerate
    )

    XCTAssertEqual(result.frequencies.count, 64)
    XCTAssertEqual(result.magnitudes.count, 64)

    // Peak bin should be the one closest to 1 kHz.
    let nearest1k = (0..<result.frequencies.count).min { lhs, rhs in
      abs(result.frequencies[lhs] - 1000) < abs(result.frequencies[rhs] - 1000)
    }!
    let peakIndex = result.magnitudes.indices.max { result.magnitudes[$0] < result.magnitudes[$1] }!
    XCTAssertEqual(peakIndex, nearest1k, "Peak should land on the bin closest to 1 kHz")

    // Full-scale sine ≈ 0 dBFS in the peak bin.
    XCTAssertLessThan(
      result.magnitudes[peakIndex], 1.0,
      "Peak should be at most 1 dBFS")
    XCTAssertGreaterThan(
      result.magnitudes[peakIndex], -10.0,
      "Peak should be within 10 dB of 0 dBFS")
  }

  func testEmptyBufferThrows() {
    let analyzer = SpectrumAnalyzer()
    let buffer = AudioHistoryBuffer()
    // No reset/ingest → buffer has no data.
    XCTAssertThrowsError(
      try analyzer.compute(
        buffer: buffer,
        channel: nil,
        minFreq: 20, maxFreq: 20000, nBins: 32,
        samplerate: 48000
      ))
  }

  func testChannelOutOfRangeThrows() {
    let analyzer = SpectrumAnalyzer()
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 2)
    buffer.append(chunk: sineChunk(freq: 440, samplerate: 48000, frames: 1024))
    XCTAssertThrowsError(
      try analyzer.compute(
        buffer: buffer,
        channel: 4,
        minFreq: 20, maxFreq: 20000, nBins: 32,
        samplerate: 48000
      ))
  }

  func testLogBinFrequenciesAreGeometric() {
    let result = logBinMagnitudes(
      power: Array(repeating: 1e-12, count: 8193),
      fftLen: 16384,
      samplerate: 48000,
      minFreq: 20,
      maxFreq: 20000,
      nBins: 5
    )
    XCTAssertEqual(result.frequencies.count, 5)
    XCTAssertEqual(Double(result.frequencies[0]), 20.0, accuracy: 1e-3)
    XCTAssertEqual(Double(result.frequencies[4]), 20000.0, accuracy: 1.0)
    // Geometric spacing: ratio between consecutive bins should be constant.
    let ratio01 = Double(result.frequencies[1] / result.frequencies[0])
    let ratio34 = Double(result.frequencies[4] / result.frequencies[3])
    XCTAssertEqual(ratio01, ratio34, accuracy: 1e-3)
  }
}
