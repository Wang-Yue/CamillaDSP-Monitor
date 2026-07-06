// Filter performance benchmarks comparing Swift throughput against CamillaDSP's reference.
// Executed in release builds via `make bench`.

import Foundation
import Testing

@testable import DSPAudio
@testable import DSPFilters

@Suite struct FilterBenchmarkTests {

  static let chunkSize = 1024
  static let sampleRate = 48000
  static let nbrFrames = 16 * chunkSize

  static var harnessBinary: String {
    if let env = ProcessInfo.processInfo.environment["CDSP_FILTER_BIN"] { return env }
    return harnessPath(named: "cdsp_filter_compare")
  }

  static func harnessPath(named name: String, file: String = #filePath) -> String {
    let url = URL(fileURLWithPath: file)
      .deletingLastPathComponent()
      .deletingLastPathComponent()
      .appendingPathComponent("RustHarnesses/target/release/\(name)")
    return url.path
  }

  private func writeRaw(_ data: [Double], to path: String) throws {
    let buf = data.withUnsafeBufferPointer { Data(buffer: $0) }
    try buf.write(to: URL(fileURLWithPath: path))
  }

  private func makeTestSignal() -> [Double] {
    var rng = SeededRNG(seed: 0xCDD5_AA42_DEAD_BEEF)
    var x = [Double](repeating: 0, count: Self.nbrFrames)
    let f1 = 200.0
    let f2 = 1500.0
    let f3 = 8000.0
    for i in 0..<x.count {
      let t = Double(i) / Double(Self.sampleRate)
      x[i] =
        0.4 * sin(2 * .pi * f1 * t) + 0.3 * sin(2 * .pi * f2 * t)
        + 0.2 * sin(2 * .pi * f3 * t) + 0.05 * (Double.random(in: -1.0...1.0, using: &rng))
    }
    return x
  }

  @Test func Convolution_Benchmark() throws {
    let label = "conv-bench"
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_conv_\(label)_in.raw"
    let outPath = "/tmp/cdsp_conv_\(label)_out.raw"
    let coeffsPath = "/tmp/cdsp_conv_\(label)_coeffs.raw"
    try writeRaw(input, to: inPath)

    var rng = SeededRNG(seed: 0xBE11C)
    let coeffs = (0..<2000).map { _ in Double.random(in: -1.0...1.0, using: &rng) }
    try writeRaw(coeffs, to: coeffsPath)

    // Measure CamillaDSP
    var cdspNsPerFrame = Double.nan
    let bin = Self.harnessBinary
    if FileManager.default.isExecutableFile(atPath: bin) {
      let proc = Process()
      proc.executableURL = URL(fileURLWithPath: bin)
      proc.arguments = [
        "conv", String(Self.chunkSize), coeffsPath, inPath, outPath,
        "--bench=2000",
      ]
      let stderr = Pipe()
      proc.standardError = stderr
      try? proc.run()
      proc.waitUntilExit()
      let stderrStr =
        String(data: stderr.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
      if proc.terminationStatus == 0 {
        var nsTotal: UInt64?
        var framesPerIter: Int?
        var iters: Int?
        for token in stderrStr.split(whereSeparator: { $0.isWhitespace }) {
          let parts = token.split(separator: "=", maxSplits: 1)
          guard parts.count == 2 else { continue }
          let k = String(parts[0])
          let v = String(parts[1])
          switch k {
          case "BENCH_NS_TOTAL": nsTotal = UInt64(v)
          case "BENCH_OUT_FRAMES_PER_ITER": framesPerIter = Int(v)
          case "BENCH_ITERS": iters = Int(v)
          default: continue
          }
        }
        if let n = nsTotal, let f = framesPerIter, let i = iters, f > 0, i > 0 {
          cdspNsPerFrame = Double(n) / Double(f * i)
        }
      } else {
        print("⚠️ CamillaDSP harness failed with status \(proc.terminationStatus): \(stderrStr)")
      }
    }

    // Measure Swift ConvolutionFilter
    let filter = ConvolutionFilter(coefficients: coeffs, chunkSize: Self.chunkSize)
    // Warm-up
    var samples = input
    var idx = 0
    while idx < samples.count {
      let end = min(idx + Self.chunkSize, samples.count)
      var slice = Array(samples[idx..<end])
      filter.process(waveform: &slice)
      idx = end
    }

    let iters = 2000
    let start = ContinuousClock.now
    for _ in 0..<iters {
      samples = input
      idx = 0
      while idx < samples.count {
        let end = min(idx + Self.chunkSize, samples.count)
        var slice = Array(samples[idx..<end])
        filter.process(waveform: &slice)
        idx = end
      }
    }
    let elapsed = ContinuousClock.now - start
    let elapsedNs =
      Double(elapsed.components.seconds) * 1e9 + Double(elapsed.components.attoseconds) * 1e-9
    let swiftNsPerFrame = elapsedNs / Double(samples.count * iters)

    print(String(format: "=== Convolution Filter Throughput ==="))
    print(String(format: "Swift ConvolutionFilter : %8.1f ns/frame", swiftNsPerFrame))
    print(String(format: "CamillaDSP FftConv      : %8.1f ns/frame", cdspNsPerFrame))
    let speedup = cdspNsPerFrame / swiftNsPerFrame
    print(String(format: "Relative Speedup        : %8.2fx", speedup))
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
