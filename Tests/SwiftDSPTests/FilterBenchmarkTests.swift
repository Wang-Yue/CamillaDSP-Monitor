// Filter performance benchmarks comparing Swift throughput against CamillaDSP's reference.
// Executed in release builds via `make bench`.

import DSPConfig
import Foundation
import Testing

@testable import SwiftDSP

@Suite(.serialized) struct FilterBenchmarkTests {

  static let chunkSize = 1024
  static let sampleRate = 48000
  static let nbrFrames = 16 * chunkSize

  static let rustResults: [String: Double] = {
    return runUpstreamFilterBenchmarks() ?? [:]
  }()

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

  private func runFilterComparison(
    label: String,
    rustName: String,
    createSwiftFilter: () -> Filter
  ) throws {
    let input = makeTestSignal()
    let filter = createSwiftFilter()

    // Warm-up
    var samples = input
    var idx = 0
    while idx < samples.count {
      let end = min(idx + Self.chunkSize, samples.count)
      var slice = Array(samples[idx..<end])
      filter.process(waveform: &slice)
      idx = end
    }

    // 200 iterations per user request
    let iters = 200
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

    let cdspNsPerFrame = Self.rustResults[rustName] ?? Double.nan

    let engineH = "Engine".padRight(toLength: 24)
    let nsPerFrameH = "ns/frame".padLeft(toLength: 15)

    print("\n" + String(repeating: "=", count: 50))
    print("Filter Benchmark: \(label)")
    print(String(repeating: "-", count: 50))
    print("\(engineH) | \(nsPerFrameH)")
    print(String(repeating: "-", count: 50))

    let swiftLabel = "Swift \(label)".padRight(toLength: 24)
    let swiftVal = String(format: "%.1f", swiftNsPerFrame).padLeft(toLength: 15)
    print("\(swiftLabel) | \(swiftVal)")

    let rustLabel = "CamillaDSP (Rust)".padRight(toLength: 24)
    let rustVal =
      !cdspNsPerFrame.isNaN
      ? String(format: "%.1f", cdspNsPerFrame).padLeft(toLength: 15) : "N/A".padLeft(toLength: 15)
    print("\(rustLabel) | \(rustVal)")
    print(String(repeating: "-", count: 50))

    if !cdspNsPerFrame.isNaN {
      let speedup = cdspNsPerFrame / swiftNsPerFrame
      print(String(format: "Relative Speedup        : %8.2fx", speedup))
    }
    print(String(repeating: "=", count: 50) + "\n")
  }

  @Test func Convolution_1024_Benchmark() throws {
    try runFilterComparison(
      label: "FftConv_1024",
      rustName: "Conv/FftConv/1024",
      createSwiftFilter: {
        let coeffs = [Double](repeating: 0.0, count: 1024)
        return try! ConvolutionFilter(coefficients: coeffs, chunkSize: Self.chunkSize)
      }
    )
  }

  @Test func Convolution_4096_Benchmark() throws {
    try runFilterComparison(
      label: "FftConv_4096",
      rustName: "Conv/FftConv/4096",
      createSwiftFilter: {
        let coeffs = [Double](repeating: 0.0, count: 4096)
        return try! ConvolutionFilter(coefficients: coeffs, chunkSize: Self.chunkSize)
      }
    )
  }

  @Test func Convolution_16384_Benchmark() throws {
    try runFilterComparison(
      label: "FftConv_16384",
      rustName: "Conv/FftConv/16384",
      createSwiftFilter: {
        let coeffs = [Double](repeating: 0.0, count: 16384)
        return try! ConvolutionFilter(coefficients: coeffs, chunkSize: Self.chunkSize)
      }
    )
  }

  @Test func Biquad_Benchmark() throws {
    try runFilterComparison(
      label: "Biquad",
      rustName: "Biquad",
      createSwiftFilter: {
        let coeffs = BiquadCoefficients(
          b0: 0.21476322779271284,
          b1: 0.4295264555854257,
          b2: 0.21476322779271284,
          a1: -0.1462978543780541,
          a2: 0.005350765548905586
        )
        return try! BiquadFilter(coefficients: coeffs)
      }
    )
  }

  @Test func DiffEq_Benchmark() throws {
    try runFilterComparison(
      label: "DiffEq",
      rustName: "DiffEq",
      createSwiftFilter: {
        var params = DiffEqParameters()
        params.a = [1.0, -0.1462978543780541, 0.005350765548905586]
        params.b = [0.21476322779271284, 0.4295264555854257, 0.21476322779271284]
        return DiffEqFilter(parameters: params)
      }
    )
  }

  static func runUpstreamFilterBenchmarks() -> [String: Double]? {
    let home = FileManager.default.homeDirectoryForCurrentUser
    let rustDir = home.appendingPathComponent("camilladsp")

    guard FileManager.default.fileExists(atPath: rustDir.path) else {
      print("⚠️ Upstream CamillaDSP repository not found at \(rustDir.path)")
      return nil
    }

    print("⏱️  Running upstream cargo bench --bench filters...")
    let proc = Process()
    proc.executableURL = URL(fileURLWithPath: "/usr/bin/env")
    proc.currentDirectoryURL = rustDir
    proc.arguments = [
      "cargo", "bench", "--bench", "filters", "--", "--sample-size", "10", "--warm-up-time", "0.3",
      "--measurement-time", "0.5",
    ]

    let pipe = Pipe()
    proc.standardOutput = pipe
    proc.standardError = Pipe()

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

    var results = [String: Double]()
    let lines = output.split(separator: "\n")

    for line in lines {
      if line.contains("time:") {
        let cleanLine = line.replacingOccurrences(of: "[", with: "").replacingOccurrences(
          of: "]", with: "")
        let parts = cleanLine.split(separator: " ").map {
          String($0).trimmingCharacters(in: .whitespacesAndNewlines)
        }.filter { !$0.isEmpty }
        if parts.count >= 6 {
          let name = parts[0]
          if let val = Double(parts[4]) {
            let unit = parts[5]
            let valNs: Double
            switch unit {
            case "ns": valNs = val
            case "µs": valNs = val * 1000.0
            case "ms": valNs = val * 1_000_000.0
            default: valNs = val
            }
            results[name] = valNs / 1024.0
          }
        }
      }
    }
    return results
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
