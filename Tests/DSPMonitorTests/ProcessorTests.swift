import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPProcessors

@Suite struct ProcessorTests {
  private static func isClose(_ left: PrcFmt, _ right: PrcFmt, maxdiff: PrcFmt) -> Bool {
    return abs(left - right) < maxdiff
  }

  @Test func compressor_basic_compression() {
    let params = CompressorParameters(
      channels: 2,
      monitorChannels: [0],
      processChannels: [0, 1],
      attack: 0.002,  // 2 ms attack
      release: 0.1,
      threshold: -6.02,  // approx 0.5 linear
      factor: 2.0,  // 2:1 compression ratio
      makeupGain: 0.0,
      softClip: false,
      clipLimit: nil
    )
    let compressor = CompressorProcessor(
      parameters: params, sampleRate: 48000, chunkSize: 1000)

    var chunk = AudioChunk(frames: 1000, channels: 2)
    for i in 0..<1000 {
      chunk[0][i] = 1.0
      chunk[1][i] = 0.5
    }

    try! compressor.process(chunk: &chunk)

    // After processing, gain is compressed.
    // Let's verify gain was attenuated for values above threshold at the end of the chunk:
    #expect(chunk[0][999] < 0.8)
    #expect(chunk[1][999] < 0.4)
  }

  @Test func noisegate_basic_gate() {
    let params = NoiseGateParameters(
      channels: 1,
      monitorChannels: [0],
      processChannels: [0],
      attack: 0.0001,  // 0.1 ms
      release: 0.0001,  // 0.1 ms
      threshold: -20.0,  // approx 0.1 linear
      attenuation: 40.0  // 40 dB attenuation
    )
    let gate = NoiseGateProcessor(
      parameters: params, sampleRate: 48000, chunkSize: 100)

    var chunk = AudioChunk(frames: 100, channels: 1)
    // 0..19: 0.001 (below threshold)
    // 20..39: 0.5 (above threshold)
    // 40..99: 0.001 (below threshold)
    for i in 0..<100 {
      if i >= 20 && i < 40 {
        chunk[0][i] = 0.5
      } else {
        chunk[0][i] = 0.001
      }
    }

    try! gate.process(chunk: &chunk)

    // At index 35, the gate should be open (gain ~1.0, so sample ~0.5)
    #expect(chunk[0][35] > 0.4)
    // At index 60, the gate should be closed (attenuated by 40 dB -> 0.01 factor, so sample 0.001 * 0.01 = 0.00001)
    #expect(chunk[0][60] < 0.00005)
  }

  @Test func race_basic() {
    let params = RACEParameters(
      channels: 2,
      channelA: 0,
      channelB: 1,
      delay: 5.0,
      subsampleDelay: false,
      delayUnit: .samples,
      attenuation: 6.02  // approx 0.5 factor
    )
    let race = try! RACEProcessor(parameters: params, sampleRate: 48000)

    var chunk = AudioChunk(frames: 10, channels: 2)
    // Send impulse to channel A
    chunk[0][0] = 1.0
    for i in 1..<10 {
      chunk[0][i] = 0.0
      chunk[1][i] = 0.0
    }

    try! race.process(chunk: &chunk)

    // With 5 sample delay (compensated to 4 samples, since 5 - 1 = 4):
    // Impulses on Channel B should appear at index 5 (feedback from A) with negative gain (-6.02 dB -> -0.5 factor, inverted is 0.5)
    // Wait, gain inverted=true means it multiplies by -0.5!
    #expect(chunk[0][0] == 1.0)
    #expect(Self.isClose(chunk[1][5], -0.5, maxdiff: 1e-4))
  }
}
