import Foundation
import Testing

@testable import DSPAudio
@testable import DSPDoP

@Suite(.serialized) struct DoPBenchmarkTests {
  @Test func DoPEncoder_Benchmark() throws {
    let carrierRate = 768_000.0  // DSD256 carrier rate
    let encoder = DoPEncoder(channels: 2, sampleRate: carrierRate, outputDoP: true)
    #expect(encoder.enabled)

    let frames = 1024
    let channels = 2
    let pcmSource = AudioChunk(frames: frames, channels: channels)
    // Fill with a 1kHz sine wave
    let amplitude: PrcFmt = 0.5
    for ch in 0..<channels {
      for t in 0..<frames {
        pcmSource[ch][t] = amplitude * PrcFmt(sin(2.0 * .pi * 1000.0 * Double(t) / carrierRate))
      }
    }

    var tempChunk = AudioChunk(frames: frames, channels: channels)

    // Warmup
    for _ in 0..<100 {
      for ch in 0..<channels {
        tempChunk[ch].baseAddress!.update(from: pcmSource[ch].baseAddress!, count: frames)
      }
      encoder.encode(chunk: &tempChunk)
    }

    // Benchmark
    let iters = 2000
    let start = ContinuousClock.now
    for _ in 0..<iters {
      for ch in 0..<channels {
        tempChunk[ch].baseAddress!.update(from: pcmSource[ch].baseAddress!, count: frames)
      }
      encoder.encode(chunk: &tempChunk)
    }
    let elapsed = ContinuousClock.now - start
    let elapsedNs =
      Double(elapsed.components.seconds) * 1e9 + Double(elapsed.components.attoseconds) * 1e-9
    let nsPerFrame = elapsedNs / Double(frames * iters)
    let realTimeRatio = (1.0 / (carrierRate * 1e-9)) / nsPerFrame

    print(String(format: "=== DoP Encoder Throughput ==="))
    print(String(format: "Throughput: %8.2f ns/frame", nsPerFrame))
    print(String(format: "Real-time ratio: %8.2fx", realTimeRatio))
  }

  @Test func DoPDecoder_Benchmark() throws {
    let carrierRate = 768_000.0  // DSD256 carrier rate
    let encoder = DoPEncoder(channels: 2, sampleRate: carrierRate, outputDoP: true)
    #expect(encoder.enabled)
    let decoder = DoPDecoder(channels: 2, sampleRate: carrierRate, bypassDoP: false)

    let frames = 1024
    let channels = 2
    let pcmSource = AudioChunk(frames: frames, channels: channels)
    // Fill with a 1kHz sine wave
    let amplitude: PrcFmt = 0.5
    for ch in 0..<channels {
      for t in 0..<frames {
        pcmSource[ch][t] = amplitude * PrcFmt(sin(2.0 * .pi * 1000.0 * Double(t) / carrierRate))
      }
    }

    // Pre-encode so we have valid DoP markers and DSD payload
    var encodedSource = pcmSource
    encoder.encode(chunk: &encodedSource)

    var tempChunk = AudioChunk(frames: frames, channels: channels)

    // Warmup and verify lock
    for _ in 0..<100 {
      for ch in 0..<channels {
        tempChunk[ch].baseAddress!.update(from: encodedSource[ch].baseAddress!, count: frames)
      }
      let processed = try decoder.detectAndProcess(chunk: &tempChunk)
      #expect(processed)
    }
    #expect(decoder.isDoPActive)

    // Benchmark
    let iters = 2000
    let start = ContinuousClock.now
    for _ in 0..<iters {
      for ch in 0..<channels {
        tempChunk[ch].baseAddress!.update(from: encodedSource[ch].baseAddress!, count: frames)
      }
      _ = try decoder.detectAndProcess(chunk: &tempChunk)
    }
    let elapsed = ContinuousClock.now - start
    let elapsedNs =
      Double(elapsed.components.seconds) * 1e9 + Double(elapsed.components.attoseconds) * 1e-9
    let nsPerFrame = elapsedNs / Double(frames * iters)
    let realTimeRatio = (1.0 / (carrierRate * 1e-9)) / nsPerFrame

    print(String(format: "=== DoP Decoder Throughput ==="))
    print(String(format: "Throughput: %8.2f ns/frame", nsPerFrame))
    print(String(format: "Real-time ratio: %8.2fx", realTimeRatio))
  }
}
