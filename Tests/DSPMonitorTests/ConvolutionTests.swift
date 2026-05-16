// ConvolutionFilter correctness tests.

import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters

@Suite struct ConvolutionTests {

  /// 2-tap moving-average IR `[0.5, 0.5]`. Chunk size matches the test
  /// vector so the entire output lands in a single block.
  @Test func MovingAverage() {
    let chunkSize = 8
    let filter = ConvolutionFilter(coefficients: [0.5, 0.5], chunkSize: chunkSize)

    var wave: [PrcFmt] = [1.0, 1.0, 1.0, 0.0, 0.0, -1.0, 0.0, 0.0]
    filter.process(waveform: &wave)

    let expected: [PrcFmt] = [0.5, 1.0, 1.0, 0.5, 0.0, -0.5, -0.5, 0.0]
    #expect(wave.count == expected.count)
    for (i, (got, exp)) in zip(wave, expected).enumerated() {
      #expect(abs(got - exp) < 1e-7, "moving avg mismatch at \(i): got \(got), expected \(exp)")
    }
  }

  /// 32-coefficient IR `0..31` with chunkSize 8 forces 4 spectrum
  /// segments. Feeding an impulse into chunk 1 should reproduce the IR
  /// across chunks 1..4 with chunk 5 going silent — exact mirror of
  /// the Rust `check_result_segmented` test.
  @Test func SegmentedConvolution() {
    let chunkSize = 8
    let ir = (0..<32).map { PrcFmt($0) }
    let filter = ConvolutionFilter(coefficients: ir, chunkSize: chunkSize)

    func runChunk(_ input: [PrcFmt], expecting expected: [PrcFmt], label: String) {
      var wave = input
      filter.process(waveform: &wave)
      for (i, (got, exp)) in zip(wave, expected).enumerated() {
        #expect(
          abs(got - exp) < 1e-5,
          "\(label) mismatch at \(i): got \(got), expected \(exp)")
      }
    }

    var impulse = [PrcFmt](repeating: 0.0, count: chunkSize)
    impulse[0] = 1.0
    runChunk(impulse, expecting: [0, 1, 2, 3, 4, 5, 6, 7], label: "chunk 1")

    let zeros = [PrcFmt](repeating: 0.0, count: chunkSize)
    runChunk(zeros, expecting: [8, 9, 10, 11, 12, 13, 14, 15], label: "chunk 2")
    runChunk(zeros, expecting: [16, 17, 18, 19, 20, 21, 22, 23], label: "chunk 3")
    runChunk(zeros, expecting: [24, 25, 26, 27, 28, 29, 30, 31], label: "chunk 4")
    runChunk(zeros, expecting: zeros, label: "chunk 5 (tail)")
  }

  /// IR = `[1.0]` is the identity; the filter must pass input through
  /// unchanged. Single-segment fast path.
  @Test func IdentityConvolution() {
    let chunkSize = 8
    let filter = ConvolutionFilter(coefficients: [1.0], chunkSize: chunkSize)

    var wave: [PrcFmt] = [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    let original = wave
    filter.process(waveform: &wave)

    for (i, (got, exp)) in zip(wave, original).enumerated() {
      #expect(abs(got - exp) < 1e-7, "identity mismatch at \(i): got \(got)")
    }
  }

  /// IR = `[0, 0, 0, 1.0]` delays the input by 3 samples. Verifies
  /// non-zero indices in a single-segment IR shift the impulse
  /// correctly.
  @Test func DelayConvolution() {
    let chunkSize = 8
    let filter = ConvolutionFilter(coefficients: [0.0, 0.0, 0.0, 1.0], chunkSize: chunkSize)

    var wave: [PrcFmt] = [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    filter.process(waveform: &wave)

    #expect(abs(wave[0]) < 1e-7)
    #expect(abs(wave[1]) < 1e-7)
    #expect(abs(wave[2]) < 1e-7)
    #expect(abs(wave[3] - 1.0) < 1e-7, "delayed impulse should appear at sample 3")
    for i in 4..<chunkSize {
      #expect(abs(wave[i]) < 1e-7, "post-impulse sample \(i) should be 0")
    }
  }

  /// Steady-state sine through a 2-tap moving-average. After the
  /// transient, the output amplitude should match the analytic
  /// frequency response of the IR within ±10%.
  @Test func ConvolutionWithSineWave() {
    let chunkSize = 64
    let sampleRate: PrcFmt = 48000.0
    let frequency: PrcFmt = 100.0
    let filter = ConvolutionFilter(coefficients: [0.5, 0.5], chunkSize: chunkSize)

    // |H(f)| for IR [0.5, 0.5] at frequency f, for a real cosine input.
    let theta = 2.0 * PrcFmt.pi * frequency / sampleRate
    let expectedGain = 0.5 * (1.0 + cos(theta))

    var lastChunk = [PrcFmt](repeating: 0.0, count: chunkSize)
    let totalChunks = 8
    for chunk in 0..<totalChunks {
      var wave = [PrcFmt](repeating: 0.0, count: chunkSize)
      let offset = chunk * chunkSize
      for i in 0..<chunkSize {
        wave[i] = cos(2.0 * PrcFmt.pi * frequency * PrcFmt(offset + i) / sampleRate)
      }
      filter.process(waveform: &wave)
      if chunk == totalChunks - 1 {
        lastChunk = wave
      }
    }

    let peak = DSPOps.peakAbsolute(lastChunk)
    #expect(
      abs(peak - expectedGain) < expectedGain * 0.10,
      "sine peak \(peak) should be within 10% of expected \(expectedGain)")
  }

  /// Empty inline IR should fail at config validation, before any
  /// filter is constructed. Exercises the `FilterFactory` →
  /// `ConvParameters.validate()` → throw path.
  @Test func EmptyIRThrows() {
    let params = ConvParameters(type: .values, values: [])
    let config = FilterConfig.conv(params)
    #expect(throws: ConfigError.self) {
      _ = try FilterFactory.create(
        config: config, sampleRate: 48000, chunkSize: 8)
    }
  }

  /// `dummy` resolves to a Kronecker delta of the requested length,
  /// which is a unit-impulse identity filter. Doubles as a smoke test
  /// for the `ConvParameters` convenience constructor path.
  @Test func DummyIsIdentity() throws {
    let chunkSize = 8
    let params = ConvParameters(type: .dummy, length: 4)
    let filter = try ConvolutionFilter(
      parameters: params, chunkSize: chunkSize, sampleRate: 48000)

    var wave: [PrcFmt] = [0.3, -0.2, 0.7, -0.1, 0.0, 0.5, -0.4, 0.9]
    let original = wave
    filter.process(waveform: &wave)

    for (i, (got, exp)) in zip(wave, original).enumerated() {
      #expect(abs(got - exp) < 1e-7, "dummy not identity at \(i)")
    }
  }
}
