// Sanity tests for the FFT spectrum analyzer. The Rust upstream tests
// (camilladsp/src/spectrum.rs) check that a 0 dBFS sine peaks near 0 dBFS;
// we replicate that and exercise the log-bin geometry and ring-buffer
// channel-out-of-range path.

import Foundation
import Testing

@testable import DSPAudio

@Suite struct SpectrumAnalyzerTests {

  /// Build a stereo `AudioChunk` of length `frames` carrying a unit-amplitude
  /// sine wave at `freq` on every channel.
  private func sineChunk(
    freq: Double, samplerate: Int, frames: Int, startFrame: Int = 0, channels: Int = 2
  )
    -> AudioChunk
  {
    let dt = 2.0 * .pi * freq / Double(samplerate)
    let waveform = (0..<frames).map { sin(dt * Double(startFrame + $0)) }
    let waveforms = Array(repeating: waveform, count: channels)
    return AudioChunk(waveforms: waveforms)
  }

  @Test func SineProducesPeakAtCarrier() throws {
    let analyzer = SpectrumAnalyzer()
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 2)
    let samplerate = 48000

    // Push enough chunks to cover the entire 8192-sample FFT window.
    for i in 0..<16 {
      buffer.append(
        chunk: sineChunk(freq: 1000, samplerate: samplerate, frames: 1024, startFrame: i * 1024))
    }

    let result = try analyzer.compute(
      buffer: buffer,
      channel: 0,
      minFreq: 20,
      maxFreq: 20000,
      nBins: 64,
      samplerate: samplerate
    )

    #expect(result.frequencies.count == 64)
    #expect(result.magnitudes.count == 64)

    // Peak bin should be the one closest to 1 kHz.
    let nearest1k = (0..<result.frequencies.count).min { lhs, rhs in
      abs(result.frequencies[lhs] - 1000) < abs(result.frequencies[rhs] - 1000)
    }!
    let peakIndex = result.magnitudes.indices.max { result.magnitudes[$0] < result.magnitudes[$1] }!
    #expect(peakIndex == nearest1k)

    // Full-scale sine ≈ 0 dBFS in the peak bin.
    #expect(result.magnitudes[peakIndex] < 1.0)
    #expect(result.magnitudes[peakIndex] > -10.0)
  }

  @Test func EmptyBufferThrows() {
    let analyzer = SpectrumAnalyzer()
    let buffer = AudioHistoryBuffer()
    // No reset/ingest → buffer has no data.
    do {
      _ = try analyzer.compute(
        buffer: buffer,
        channel: nil,
        minFreq: 20, maxFreq: 20000, nBins: 32,
        samplerate: 48000
      )
      Issue.record("Expected error to be thrown")
    } catch {
      // expected exception
    }
  }

  @Test func ChannelOutOfRangeThrows() {
    let analyzer = SpectrumAnalyzer()
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 2)
    buffer.append(chunk: sineChunk(freq: 440, samplerate: 48000, frames: 1024))
    do {
      _ = try analyzer.compute(
        buffer: buffer,
        channel: 4,
        minFreq: 20, maxFreq: 20000, nBins: 32,
        samplerate: 48000
      )
      Issue.record("Expected error to be thrown")
    } catch {
      // expected exception
    }
  }

  @Test func LogBinFrequenciesAreGeometric() throws {
    let analyzer = SpectrumAnalyzer()
    let buffer = AudioHistoryBuffer()
    buffer.reset(channels: 1)
    buffer.append(chunk: AudioChunk(frames: 4096, channels: 1))

    let result = try analyzer.compute(
      buffer: buffer,
      channel: 0,
      minFreq: 20,
      maxFreq: 20000,
      nBins: 5,
      samplerate: 48000
    )
    #expect(result.frequencies.count == 5)
    #expect(abs(Double(result.frequencies[0]) - 20.0) <= 1e-3)
    #expect(abs(Double(result.frequencies[4]) - 20000.0) <= 1.0)
    // Geometric spacing: ratio between consecutive bins should be constant.
    let ratio01 = Double(result.frequencies[1] / result.frequencies[0])
    let ratio34 = Double(result.frequencies[4] / result.frequencies[3])
    #expect((ratio01 - ratio34).magnitude <= 1e-3)
  }
}
