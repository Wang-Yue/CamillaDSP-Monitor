// Cross-language performance comparison: Swift CamillaDSP-Monitor resamplers
// vs the rubato Rust reference. Both run the same input through the same
// algorithm `iters` times back-to-back; the harness reports its own elapsed
// time on stderr. Prints a side-by-side ns/output-frame breakdown so we can
// catch performance regressions in the Swift port.
//
// Skipped when the Rust harness binary is missing — like the other
// comparison tests, this lets ordinary CI runs without Cargo skip cleanly.

import Foundation
import XCTest

@testable import CamillaDSPLib

final class ResamplerCrossLanguagePerfTests: XCTestCase {

  static let inRate = 44100
  static let outRate = 48000
  static let chunkSize = 1024
  /// 64 chunks × 1024 = 65536 frames ≈ 1.5 s of audio at 44.1 kHz. Big enough
  /// that per-iteration overhead is negligible, small enough that the Rust
  /// harness invocation stays under a second even at 30 iters.
  static let totalChunks = 64
  static let bencheIters = 200

  static var rubatoBinary: String {
    if let env = ProcessInfo.processInfo.environment["RUBATO_BIN"] { return env }
    return ResamplerComparisonTests.harnessBinary(named: "cdsp_resampler_compare")
  }

  // MARK: - Tests

  func testPerf_AsyncSincAccurate() throws {
    try runComparison(
      mode: "sinc-accurate",
      label: "AsyncSinc Accurate",
      makeSwift: {
        AsyncSincResampler(
          channels: 1, inputRate: Self.inRate, outputRate: Self.outRate,
          profile: .accurate, chunkSize: Self.chunkSize)
      })
  }

  func testPerf_AsyncPolyCubic() throws {
    try runComparison(
      mode: "poly-cubic",
      label: "AsyncPoly Cubic",
      makeSwift: {
        AsyncPolyResampler(
          channels: 1, inputRate: Self.inRate, outputRate: Self.outRate,
          interpolation: .cubic, chunkSize: Self.chunkSize)
      })
  }

  func testPerf_AsyncPolySeptic() throws {
    try runComparison(
      mode: "poly-septic",
      label: "AsyncPoly Septic",
      makeSwift: {
        AsyncPolyResampler(
          channels: 1, inputRate: Self.inRate, outputRate: Self.outRate,
          interpolation: .septic, chunkSize: Self.chunkSize)
      })
  }

  func testPerf_SynchronousFft() throws {
    try runComparison(
      mode: "fft",
      label: "Synchronous FFT",
      makeSwift: {
        SynchronousResampler(
          channels: 1, inputRate: Self.inRate, outputRate: Self.outRate,
          chunkSize: Self.chunkSize)
      })
  }

  // MARK: - Comparison driver

  private func runComparison(
    mode: String, label: String, makeSwift: () -> AudioResampler
  ) throws {
    // Random f64 input shared between both sides — same bits go through
    // Swift and through the Rust harness.
    var rng = SystemRandomNumberGenerator()
    let nbrIn = Self.totalChunks * Self.chunkSize
    var input = [Double](repeating: 0, count: nbrIn)
    for i in 0..<nbrIn { input[i] = Double.random(in: -1.0...1.0, using: &rng) }

    let inPath = "/tmp/cdsp_perf_\(mode)_in.raw"
    try writeRaw(input, to: inPath)

    let bin = Self.rubatoBinary
    guard FileManager.default.isExecutableFile(atPath: bin) else {
      throw XCTSkip("rubato harness not found at \(bin)")
    }

    // Swift bench: same loop the harness runs — all chunks one full sweep,
    // repeated `iters` times. One warm-up sweep first.
    let resampler = makeSwift()
    let cs = resampler.chunkSize
    let chunkCount = nbrIn / cs

    var chunks: [AudioChunk] = []
    chunks.reserveCapacity(chunkCount)
    for c in 0..<chunkCount {
      let slice = Array(input[c * cs..<(c + 1) * cs])
      chunks.append(AudioChunk(waveforms: [slice], validFrames: cs))
    }

    let maxOut = resampler.maxOutputFrames
    var scratch = AudioChunk(
      waveforms: [[Double](repeating: 0, count: maxOut)],
      validFrames: 0)

    // Warm-up sweep so lastIndex / overlap state has settled before timing.
    for c in chunks {
      try resampler.process(input: c, into: &scratch)
    }

    // Timed loop. We sum the actual output-frame count across iterations
    // rather than assuming steady state — async resamplers' last_index can
    // drift by a frame between sweeps as they cycle through the same input.
    let swiftStart = ContinuousClock.now
    var swiftOutFramesTotal = 0
    for _ in 0..<Self.bencheIters {
      for c in chunks {
        try resampler.process(input: c, into: &scratch)
        swiftOutFramesTotal += scratch.validFrames
      }
    }
    let swiftElapsed = ContinuousClock.now - swiftStart
    let swiftElapsedNs =
      Double(swiftElapsed.components.seconds) * 1e9
      + Double(swiftElapsed.components.attoseconds) * 1e-9
    let swiftNsPerOut = swiftElapsedNs / Double(swiftOutFramesTotal)

    // Rust bench via the harness (--bench=N).
    let outPath = "/tmp/cdsp_perf_\(mode)_out.raw"
    let proc = Process()
    proc.executableURL = URL(fileURLWithPath: bin)
    proc.arguments = [
      mode, inPath, outPath,
      String(Self.inRate), String(Self.outRate), String(Self.chunkSize),
      "--bench=\(Self.bencheIters)",
    ]
    let stderr = Pipe()
    proc.standardError = stderr
    try proc.run()
    proc.waitUntilExit()

    let stderrData = stderr.fileHandleForReading.readDataToEndOfFile()
    let stderrStr = String(data: stderrData, encoding: .utf8) ?? ""
    XCTAssertEqual(
      proc.terminationStatus, 0,
      "rubato harness failed: \(stderrStr)")

    guard let bench = parseBench(stderrStr) else {
      XCTFail("could not parse bench output: \(stderrStr)")
      return
    }
    let rustNsPerOut = Double(bench.nsTotal) / Double(bench.outFramesPerIter * bench.iters)

    // Per-iter throughput as multiples of real-time.
    let inSec = Double(nbrIn) / Double(Self.inRate)
    let swiftRtfPerIter = inSec / (swiftElapsedNs * 1e-9 / Double(Self.bencheIters))
    let rustRtfPerIter = inSec / (Double(bench.nsTotal) * 1e-9 / Double(bench.iters))
    let speedRatio = rustNsPerOut / swiftNsPerOut

    print(
      String(
        format:
          "[%@]  Swift: %.1f ns/outFrame  RTF=%.1fx  |  Rust: %.1f ns/outFrame  RTF=%.1fx  |  Swift/Rust = %.2fx (>1.0 means Swift is faster)",
        label, swiftNsPerOut, swiftRtfPerIter,
        rustNsPerOut, rustRtfPerIter, speedRatio))

    // Sanity: Swift must clear real-time. Debug builds are 30-40× slower than
    // release on this code (no inlining, no vectorisation), so the floor here
    // is conservative — the headline numbers above are what we actually care
    // about; this assert just catches gross regressions.
    XCTAssertGreaterThan(
      swiftRtfPerIter, 1.5,
      "[\(label)] Swift RTF=\(swiftRtfPerIter)x is below real-time — likely a perf regression")
  }

  // MARK: - Helpers

  private struct BenchOutput {
    let nsTotal: UInt64
    let outFramesPerIter: Int
    let iters: Int
    let mode: String
  }

  private func parseBench(_ s: String) -> BenchOutput? {
    // Expected line:
    //   BENCH_NS_TOTAL=...  BENCH_OUT_FRAMES_PER_ITER=...  BENCH_ITERS=...  BENCH_MODE=...
    var nsTotal: UInt64?
    var outFrames: Int?
    var iters: Int?
    var mode: String?
    for token in s.split(whereSeparator: { $0.isWhitespace }) {
      let parts = token.split(separator: "=", maxSplits: 1)
      guard parts.count == 2 else { continue }
      let k = String(parts[0])
      let v = String(parts[1])
      switch k {
      case "BENCH_NS_TOTAL": nsTotal = UInt64(v)
      case "BENCH_OUT_FRAMES_PER_ITER": outFrames = Int(v)
      case "BENCH_ITERS": iters = Int(v)
      case "BENCH_MODE": mode = v
      default: continue
      }
    }
    guard let n = nsTotal, let o = outFrames, let i = iters, let m = mode else {
      return nil
    }
    return BenchOutput(nsTotal: n, outFramesPerIter: o, iters: i, mode: m)
  }

  private func writeRaw(_ data: [Double], to path: String) throws {
    let buffer = data.withUnsafeBufferPointer { Data(buffer: $0) }
    try buffer.write(to: URL(fileURLWithPath: path))
  }
}
