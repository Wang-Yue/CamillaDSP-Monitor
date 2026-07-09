import Foundation
import Testing

@testable import DSPConfig
@testable import SwiftDSP

@Suite(.serialized) struct PipelineBenchmarkTests {
  static let sampleRate = 48000
  static let chunkSize = 1024

  private func makeDummySignal(channels: Int) -> AudioChunk {
    let chunk = AudioChunk(frames: Self.chunkSize, channels: channels)
    for ch in 0..<channels {
      for t in 0..<Self.chunkSize {
        chunk[ch][t] = sin(2.0 * .pi * 1000.0 * Double(t) / Double(Self.sampleRate))
      }
    }
    return chunk
  }

  // PRE_BIQUAD_PARAMS
  static let preBiquadParams: [(freq: Double, q: Double)] = [
    (120.0, 0.70), (220.0, 0.75), (350.0, 0.80), (500.0, 0.90),
    (700.0, 1.00), (900.0, 1.10), (1200.0, 0.95), (1600.0, 1.05),
    (1800.0, 1.10), (2200.0, 0.90), (2800.0, 0.95), (3200.0, 1.00),
    (3800.0, 0.85), (4500.0, 0.80), (6200.0, 0.75), (8000.0, 0.70),
  ]

  // POST_BIQUAD_PARAMS
  static let postBiquadParams: [(freq: Double, q: Double)] = [
    (140.0, 0.72), (260.0, 0.78), (400.0, 0.83), (560.0, 0.92),
    (760.0, 1.02), (980.0, 1.08), (1300.0, 0.98), (1700.0, 1.06),
    (2100.0, 1.00), (2500.0, 0.94), (3000.0, 0.92), (3600.0, 0.88),
    (4200.0, 0.84), (5200.0, 0.80), (6800.0, 0.76), (9200.0, 0.72),
  ]

  private func buildConvFilterCoefficients(length: Int) -> [Double] {
    var values = [Double]()
    values.reserveCapacity(length)
    let pi = Double.pi
    for idx in 0..<length {
      let x = Double(idx) - Double(length - 1) * 0.5
      let sinc = x == 0.0 ? 1.0 : sin(pi * x) / (pi * x)
      values.append(sinc)
    }
    return values
  }

  @Test func Pipeline_UpstreamMatch_Biquads_Benchmark() throws {
    var config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: Self.sampleRate, chunksize: Self.chunkSize,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 4),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))

    var filters: [String: FilterConfig] = [:]
    var preFilterNames: [String] = []
    for (i, p) in Self.preBiquadParams.enumerated() {
      var params = BiquadParameters()
      params.type = .peaking
      params.freq = p.freq
      params.q = p.q
      params.gain = 1.5
      let name = "pre_bq_\(i + 1)"
      filters[name] = .biquad(params)
      preFilterNames.append(name)
    }

    var postFilterNames: [String] = []
    for (i, p) in Self.postBiquadParams.enumerated() {
      var params = BiquadParameters()
      params.type = .peaking
      params.freq = p.freq
      params.q = p.q
      params.gain = 1.5
      let name = "post_bq_\(i + 1)"
      filters[name] = .biquad(params)
      postFilterNames.append(name)
    }
    config.filters = filters

    // Mixer
    let map0 = MixerMapping(
      dest: 0,
      sources: [
        MixerSource(channel: 0, gain: 0.0, inverted: false, mute: false, scale: .dB),
        MixerSource(channel: 2, gain: -6.0, inverted: false, mute: false, scale: .dB),
      ])
    let map1 = MixerMapping(
      dest: 1,
      sources: [
        MixerSource(channel: 1, gain: 0.0, inverted: false, mute: false, scale: .dB),
        MixerSource(channel: 3, gain: -6.0, inverted: false, mute: false, scale: .dB),
      ])
    config.mixers = [
      "mix_4_to_2": MixerConfig(channelsIn: 4, channelsOut: 2, mapping: [map0, map1])
    ]

    // Pipeline Steps
    config.pipeline = [
      PipelineStep(type: .filter, channels: nil, names: preFilterNames),
      PipelineStep(type: .mixer, name: "mix_4_to_2"),
      PipelineStep(type: .filter, channels: nil, names: postFilterNames),
    ]

    try runComparison(
      config: config,
      label: "Upstream Match: 4-in 2-out Biquad Pipeline (96 EQ evaluations)",
      inputChannels: 4,
      outputChannels: 2,
      iters: 200,
      rustVariantSingle: "biquad_single",
      rustVariantMulti: "biquad_multi"
    )
  }

  @Test func Pipeline_UpstreamMatch_Biquads_Conv_Benchmark() throws {
    var config = DSPConfiguration(
      devices: DevicesConfig(
        samplerate: Self.sampleRate, chunksize: Self.chunkSize,
        capture: CaptureDeviceConfig(type: .coreAudio, channels: 4),
        playback: PlaybackDeviceConfig(type: .coreAudio, channels: 2)))

    var filters: [String: FilterConfig] = [:]
    var preFilterNames: [String] = []
    for (i, p) in Self.preBiquadParams.enumerated() {
      var params = BiquadParameters()
      params.type = .peaking
      params.freq = p.freq
      params.q = p.q
      params.gain = 1.5
      let name = "pre_bq_\(i + 1)"
      filters[name] = .biquad(params)
      preFilterNames.append(name)
    }

    var postFilterNames: [String] = []
    for (i, p) in Self.postBiquadParams.enumerated() {
      var params = BiquadParameters()
      params.type = .peaking
      params.freq = p.freq
      params.q = p.q
      params.gain = 1.5
      let name = "post_bq_\(i + 1)"
      filters[name] = .biquad(params)
      postFilterNames.append(name)
    }

    // Add convolutions: pre-conv 1 (32768), pre-conv 2 (65536)
    let preCoeffs1 = buildConvFilterCoefficients(length: 32768)
    let preCoeffs2 = buildConvFilterCoefficients(length: 65536)
    filters["pre_conv_1"] = .conv(ConvParameters(type: .values, values: preCoeffs1))
    filters["pre_conv_2"] = .conv(ConvParameters(type: .values, values: preCoeffs2))
    preFilterNames.append("pre_conv_1")
    preFilterNames.append("pre_conv_2")

    // Add convolutions: post-conv 1 (32768), post-conv 2 (65536)
    let postCoeffs1 = buildConvFilterCoefficients(length: 32768)
    let postCoeffs2 = buildConvFilterCoefficients(length: 65536)
    filters["post_conv_1"] = .conv(ConvParameters(type: .values, values: postCoeffs1))
    filters["post_conv_2"] = .conv(ConvParameters(type: .values, values: postCoeffs2))
    postFilterNames.append("post_conv_1")
    postFilterNames.append("post_conv_2")

    config.filters = filters

    // Mixer
    let map0 = MixerMapping(
      dest: 0,
      sources: [
        MixerSource(channel: 0, gain: 0.0, inverted: false, mute: false, scale: .dB),
        MixerSource(channel: 2, gain: -6.0, inverted: false, mute: false, scale: .dB),
      ])
    let map1 = MixerMapping(
      dest: 1,
      sources: [
        MixerSource(channel: 1, gain: 0.0, inverted: false, mute: false, scale: .dB),
        MixerSource(channel: 3, gain: -6.0, inverted: false, mute: false, scale: .dB),
      ])
    config.mixers = [
      "mix_4_to_2": MixerConfig(channelsIn: 4, channelsOut: 2, mapping: [map0, map1])
    ]

    // Pipeline Steps
    config.pipeline = [
      PipelineStep(type: .filter, channels: nil, names: preFilterNames),
      PipelineStep(type: .mixer, name: "mix_4_to_2"),
      PipelineStep(type: .filter, channels: nil, names: postFilterNames),
    ]

    try runComparison(
      config: config,
      label: "Upstream Match: 4-in 2-out Biquad + Convolution Pipeline (96 EQ + 12 long convolve)",
      inputChannels: 4,
      outputChannels: 2,
      iters: 10,
      rustVariantSingle: "biquad_conv_single",
      rustVariantMulti: "biquad_conv_multi"
    )
  }

  private func runComparison(
    config: DSPConfiguration,
    label: String,
    inputChannels: Int,
    outputChannels: Int,
    iters: Int = 2000,
    rustVariantSingle: String,
    rustVariantMulti: String
  ) throws {
    let params = ProcessingParameters(
      captureChannels: inputChannels, playbackChannels: outputChannels)

    let pipelineSingle = try Pipeline(
      config: config, processingParams: params, mode: .singleThreaded)
    let pipelineMulti = try Pipeline(config: config, processingParams: params, mode: .multiThreaded)

    let input = makeDummySignal(channels: inputChannels)
    var output = AudioChunk(frames: Self.chunkSize, channels: outputChannels)

    // Warm-up
    for _ in 0..<50 {
      try pipelineSingle.process(input: input, into: &output)
      try pipelineMulti.process(input: input, into: &output)
    }

    // Benchmark Single-Threaded
    let startSingle = ContinuousClock.now
    for _ in 0..<iters {
      try pipelineSingle.process(input: input, into: &output)
    }
    let elapsedSingle = ContinuousClock.now - startSingle
    let singleNs =
      Double(elapsedSingle.components.seconds) * 1e9 + Double(elapsedSingle.components.attoseconds)
      * 1e-9

    // Benchmark Multi-Threaded
    let startMulti = ContinuousClock.now
    for _ in 0..<iters {
      try pipelineMulti.process(input: input, into: &output)
    }
    let elapsedMulti = ContinuousClock.now - startMulti
    let multiNs =
      Double(elapsedMulti.components.seconds) * 1e9 + Double(elapsedMulti.components.attoseconds)
      * 1e-9

    let swiftSingleMs = singleNs / Double(iters) / 1e6
    let swiftMultiMs = multiNs / Double(iters) / 1e6

    let rust = runUpstreamBenchmark(
      variantSingle: rustVariantSingle, variantMulti: rustVariantMulti)

    let engineH = "Engine".padRight(toLength: 17)
    let singleH = "Single (ms)".padLeft(toLength: 12)
    let multiH = "Multi (ms)".padLeft(toLength: 12)
    let speedupH = "Speedup".padLeft(toLength: 11)

    print("\n" + String(repeating: "=", count: 80))
    print("Benchmark: \(label)")
    print(String(repeating: "-", count: 80))
    print("\(engineH) | \(singleH) | \(multiH) | \(speedupH)")
    print(String(repeating: "-", count: 80))

    if let rust = rust {
      let rustRatio = rust.singleMs / rust.multiMs
      let name = "CamillaDSP (Rust)".padRight(toLength: 17)
      let single = String(format: "%.3f", rust.singleMs).padLeft(toLength: 12)
      let multi = String(format: "%.3f", rust.multiMs).padLeft(toLength: 12)
      let speedup = String(format: "%.2fx", rustRatio).padLeft(toLength: 11)
      print("\(name) | \(single) | \(multi) | \(speedup)")
    } else {
      let name = "CamillaDSP (Rust)".padRight(toLength: 17)
      let single = "N/A".padLeft(toLength: 12)
      let multi = "N/A".padLeft(toLength: 12)
      let speedup = "N/A".padLeft(toLength: 11)
      print("\(name) | \(single) | \(multi) | \(speedup)")
    }

    let swiftRatio = swiftSingleMs / swiftMultiMs
    let name = "SwiftDSP (Swift)".padRight(toLength: 17)
    let single = String(format: "%.3f", swiftSingleMs).padLeft(toLength: 12)
    let multi = String(format: "%.3f", swiftMultiMs).padLeft(toLength: 12)
    let speedup = String(format: "%.2fx", swiftRatio).padLeft(toLength: 11)
    print("\(name) | \(single) | \(multi) | \(speedup)")
    print(String(repeating: "-", count: 80))

    if let rust = rust {
      let winner: String
      let factor: Double
      if swiftMultiMs < rust.multiMs {
        winner = "SwiftDSP (Swift)"
        factor = rust.multiMs / swiftMultiMs
      } else {
        winner = "CamillaDSP (Rust)"
        factor = swiftMultiMs / rust.multiMs
      }
      print(
        "Head-to-Head      | \(winner) is \(String(format: "%.2f", factor))x faster in multi-threaded mode"
      )
    }
    print(String(repeating: "=", count: 80) + "\n")
  }

  private func runUpstreamBenchmark(
    variantSingle: String,
    variantMulti: String
  ) -> (singleMs: Double, multiMs: Double)? {
    let home = FileManager.default.homeDirectoryForCurrentUser
    let rustDir = home.appendingPathComponent("camilladsp")

    guard FileManager.default.fileExists(atPath: rustDir.path) else {
      print("⚠️ Upstream CamillaDSP repository not found at \(rustDir.path)")
      return nil
    }

    print("⏱️  Running upstream cargo bench --bench pipeline...")
    let proc = Process()
    proc.executableURL = URL(fileURLWithPath: "/usr/bin/env")
    proc.currentDirectoryURL = rustDir
    proc.arguments = [
      "cargo", "bench", "--bench", "pipeline", "--", "--sample-size", "10", "--warm-up-time", "0.3",
      "--measurement-time", "0.5",
    ]

    let pipe = Pipe()
    proc.standardOutput = pipe
    proc.standardError = Pipe()  // suppress cargo build warnings

    do {
      try proc.run()
      proc.waitUntilExit()
    } catch {
      print("⚠️ Failed to run upstream benchmark process: \(error)")
      return nil
    }

    guard proc.terminationStatus == 0 else {
      print("⚠️ Upstream cargo bench exited with status \(proc.terminationStatus)")
      return nil
    }

    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    guard let output = String(data: data, encoding: .utf8) else {
      return nil
    }

    var singleMs = Double.nan
    var multiMs = Double.nan

    let lines = output.split(separator: "\n")
    var lastVariant: String? = nil
    for line in lines {
      if line.contains("complete_pipeline_chunk/variant/") {
        if let last = line.split(separator: "/").last {
          lastVariant = last.trimmingCharacters(in: .whitespacesAndNewlines)
        }
      } else if line.contains("time:") && lastVariant != nil {
        let cleanLine = line.replacingOccurrences(of: "[", with: "").replacingOccurrences(
          of: "]", with: "")
        let parts = cleanLine.split(separator: " ").map { String($0) }
        if parts.count >= 5 {
          if let val = Double(parts[3]) {
            let unit = parts[4]
            let valMs = unit == "µs" ? val / 1000.0 : val

            if lastVariant == variantSingle {
              singleMs = valMs
            } else if lastVariant == variantMulti {
              multiMs = valMs
            }
          }
        }
        lastVariant = nil
      }
    }

    if singleMs.isNaN || multiMs.isNaN {
      print(
        "⚠️ Could not parse mean times for variants \(variantSingle) and \(variantMulti) from cargo bench output."
      )
      return nil
    }

    return (singleMs, multiMs)
  }
}

private struct SeededRNG: RandomNumberGenerator {
  var state: UInt64
  init(seed: UInt64) { self.state = seed }
  mutating func next() -> UInt64 {
    state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
    return state
  }
}

extension String {
  fileprivate func padRight(toLength length: Int) -> String {
    return self.padding(toLength: length, withPad: " ", startingAt: 0)
  }

  fileprivate func padLeft(toLength length: Int) -> String {
    if self.count >= length { return self }
    return String(repeating: " ", count: length - self.count) + self
  }
}
