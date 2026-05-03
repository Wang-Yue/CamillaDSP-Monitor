// Numerical comparison of Swift's `AsyncSincResampler` (Accurate profile) against
// the Rust rubato reference. Drives the fixes described in the audit.
//
// To use this test, first build the rubato reference binary:
//   cd ~/rubato && cargo build --release --example cdsp_compare
//
// The binary defaults to ~/rubato/target/release/examples/cdsp_compare; override
// with the env var RUBATO_BIN if needed. Tests are skipped (with a warning) when
// the binary is missing, so they don't break ordinary CI runs that lack Rust.

import Foundation
import XCTest

@testable import CamillaDSPLib

final class ResamplerComparisonTests: XCTestCase {

  // MARK: - Test parameters (Accurate profile, 44.1 → 48 kHz)
  static let inRate = 44100
  static let outRate = 48000
  static let chunkSize = 1024

  static var rubatoBinary: String {
    if let env = ProcessInfo.processInfo.environment["RUBATO_BIN"] { return env }
    return Self.harnessBinary(named: "cdsp_resampler_compare")
  }

  /// Locate a Rust harness binary alongside this test file. The harnesses live
  /// at `Tests/RustHarnesses/target/release/<name>` relative to the project
  /// root; we walk up from `#filePath` (the location of this Swift source) to
  /// find the project root deterministically, regardless of `swift test`'s
  /// working directory.
  static func harnessBinary(named name: String, file: String = #filePath) -> String {
    // file = .../CamillaDSP-Monitor/Tests/CamillaDSPLibTests/ResamplerComparisonTests.swift
    // walk up two levels to land at .../CamillaDSP-Monitor/Tests
    let url = URL(fileURLWithPath: file)
      .deletingLastPathComponent()  // CamillaDSPLibTests
      .deletingLastPathComponent()  // Tests
      .appendingPathComponent("RustHarnesses/target/release/\(name)")
    return url.path
  }

  // MARK: - I/O helpers

  private func writeRaw(_ data: [Double], to path: String) throws {
    let buffer = data.withUnsafeBufferPointer { Data(buffer: $0) }
    try buffer.write(to: URL(fileURLWithPath: path))
  }

  private func readRaw(from path: String) throws -> [Double] {
    let data = try Data(contentsOf: URL(fileURLWithPath: path))
    let count = data.count / MemoryLayout<Double>.stride
    return data.withUnsafeBytes { raw -> [Double] in
      let p = raw.bindMemory(to: Double.self)
      return Array(UnsafeBufferPointer(start: p.baseAddress, count: count))
    }
  }

  /// Runs the rubato CLI in a given mode. Returns false if the binary is missing
  /// (test should `throw XCTSkip` in that case).
  private func runRubato(
    mode: String, input: String, output: String,
    inRate: Int = inRate, outRate: Int = outRate, chunkSize: Int = chunkSize
  ) throws -> Bool {
    let bin = Self.rubatoBinary
    guard FileManager.default.isExecutableFile(atPath: bin) else {
      print(
        "⚠️ skipping: rubato binary not found at \(bin) — build with `cargo build --release --example cdsp_compare`"
      )
      return false
    }
    let proc = Process()
    proc.executableURL = URL(fileURLWithPath: bin)
    proc.arguments = [
      mode, input, output,
      String(inRate), String(outRate), String(chunkSize),
    ]
    let stderr = Pipe()
    proc.standardError = stderr
    try proc.run()
    proc.waitUntilExit()
    if proc.terminationStatus != 0 {
      let err =
        String(data: stderr.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
      XCTFail("rubato binary exited with \(proc.terminationStatus): \(err)")
      return false
    }
    return true
  }

  // MARK: - Run Swift resamplers over a flat input array

  private func runSwiftResampler(_ resampler: AudioResampler, input: [Double]) -> [Double] {
    var output: [Double] = []
    output.reserveCapacity(
      Int(Double(input.count) * Double(Self.outRate) / Double(Self.inRate)) + 64)
    // Use the resampler's actual chunkSize — `SynchronousResampler` rounds it
    // up to the smallest valid FFT-compatible multiple, so it may differ from
    // the constructor hint.
    let cs = resampler.chunkSize
    var idx = 0
    while idx + cs <= input.count {
      let slice = Array(input[idx..<idx + cs])
      let chunk = AudioChunk(waveforms: [slice], validFrames: cs)
      let out = try! resampler.process(chunk: chunk)
      output.append(contentsOf: out.waveforms[0][0..<out.validFrames])
      idx += cs
    }
    return output
  }

  private func runAccurateSinc(input: [Double]) -> [Double] {
    runSwiftResampler(
      AsyncSincResampler(
        channels: 1, inputRate: Self.inRate, outputRate: Self.outRate,
        profile: .accurate, chunkSize: Self.chunkSize),
      input: input)
  }

  private func runPoly(_ interp: PolyInterpolation, input: [Double]) -> [Double] {
    runSwiftResampler(
      AsyncPolyResampler(
        channels: 1, inputRate: Self.inRate, outputRate: Self.outRate,
        interpolation: interp, chunkSize: Self.chunkSize),
      input: input)
  }

  private func runSync(input: [Double]) -> [Double] {
    runSwiftResampler(
      SynchronousResampler(
        channels: 1, inputRate: Self.inRate, outputRate: Self.outRate, chunkSize: Self.chunkSize),
      input: input)
  }

  // MARK: - Diagnostic helpers

  /// Search lag in [-maxLag, +maxLag] (Swift relative to ref) that minimises RMS error
  /// on `length` samples beginning at `skip` in the reference. Negative lag means the
  /// Swift output is delayed relative to rubato.
  private func findBestAlignment(
    swift: [Double], ref: [Double], skip: Int, length: Int, maxLag: Int
  ) -> (lag: Int, rms: Double, maxDiff: Double) {
    var bestLag = 0
    var bestRms = Double.infinity
    var bestMax = 0.0
    for lag in -maxLag...maxLag {
      var sumSq = 0.0
      var maxAbs = 0.0
      var counted = 0
      for i in 0..<length {
        let r = skip + i
        let s = r + lag
        if r < 0 || s < 0 || r >= ref.count || s >= swift.count { continue }
        let d = swift[s] - ref[r]
        sumSq += d * d
        if abs(d) > maxAbs { maxAbs = abs(d) }
        counted += 1
      }
      if counted == 0 { continue }
      let rms = sqrt(sumSq / Double(counted))
      if rms < bestRms {
        bestRms = rms
        bestLag = lag
        bestMax = maxAbs
      }
    }
    return (bestLag, bestRms, bestMax)
  }

  private func absPeak(_ x: [Double]) -> (idx: Int, val: Double) {
    var bi = 0
    var bv = 0.0
    for (i, v) in x.enumerated() where abs(v) > abs(bv) {
      bi = i
      bv = v
    }
    return (bi, bv)
  }

  // MARK: - Tests

  // MARK: - AsyncSinc Accurate vs rubato sinc-accurate

  func testAccurate_Sine1kHz_44100to48000() throws {
    let nbrIn = 32 * Self.chunkSize  // 32768 frames so we have steady-state output
    var input = [Double](repeating: 0, count: nbrIn)
    let omega = 2.0 * .pi * 1000.0 / Double(Self.inRate)
    for i in 0..<nbrIn { input[i] = sin(omega * Double(i)) }

    let inPath = "/tmp/cdsp_sine_in.raw"
    let refPath = "/tmp/cdsp_sine_ref.raw"
    try writeRaw(input, to: inPath)
    guard try runRubato(mode: "sinc-accurate", input: inPath, output: refPath) else {
      throw XCTSkip("rubato binary missing")
    }
    let ref = try readRaw(from: refPath)
    let swiftOut = runAccurateSinc(input: input)

    XCTAssertGreaterThan(ref.count, 5000)
    XCTAssertGreaterThan(swiftOut.count, 5000)

    // Swift's AsyncSinc now matches rubato's `Async::new_sinc` algorithm
    // exactly — same buffer layout, same `last_index` semantics, same kernel
    // construction. Output should agree to floating-point noise.
    let result = findBestAlignment(
      swift: swiftOut, ref: ref, skip: 4000, length: 16384, maxLag: 8)
    print(
      String(
        format:
          "[sine 1kHz] swift=%d ref=%d  bestLag=%d  rms=%.3e  maxDiff=%.3e",
        swiftOut.count, ref.count, result.lag, result.rms, result.maxDiff))
    XCTAssertEqual(result.lag, 0, "AsyncSinc should align with rubato at lag 0")
    // Bit-equivalent — last sub-ULP wiggle is from libm cos rounding inside
    // the kernel build versus rubato's. RMS ≤ 5 · 2⁻⁵³ ≈ 5e-16 is the
    // tightest stable bound across runs.
    XCTAssertLessThan(
      result.rms, 5e-16, "AsyncSinc sine output diverges from rubato beyond FP noise")
  }

  func testAccurate_Impulse_44100to48000() throws {
    let nbrIn = 32 * Self.chunkSize
    var input = [Double](repeating: 0, count: nbrIn)
    let impulseAt = 1000
    input[impulseAt] = 1.0

    let inPath = "/tmp/cdsp_imp_in.raw"
    let refPath = "/tmp/cdsp_imp_ref.raw"
    try writeRaw(input, to: inPath)
    guard try runRubato(mode: "sinc-accurate", input: inPath, output: refPath) else {
      throw XCTSkip("rubato binary missing")
    }
    let ref = try readRaw(from: refPath)
    let swiftOut = runAccurateSinc(input: input)

    let swiftPeak = absPeak(Array(swiftOut.prefix(8000)))
    let refPeak = absPeak(Array(ref.prefix(8000)))

    print(
      String(
        format: "[impulse] swiftPeak idx=%d val=%.6f  refPeak idx=%d val=%.6f",
        swiftPeak.idx, swiftPeak.val, refPeak.idx, refPeak.val))
    XCTAssertEqual(swiftPeak.idx, refPeak.idx, "AsyncSinc impulse peak should match rubato exactly")

    // After matching rubato algorithmically, the impulse response should be
    // bit-identical sample-by-sample (modulo a few ULPs of FMA noise).
    let win = 256
    let lag = swiftPeak.idx - refPeak.idx
    var sumSq = 0.0
    var maxAbs = 0.0
    var counted = 0
    for k in -win...win {
      let r = refPeak.idx + k
      let s = r + lag
      if r < 0 || s < 0 || r >= ref.count || s >= swiftOut.count { continue }
      let d = swiftOut[s] - ref[r]
      sumSq += d * d
      if abs(d) > maxAbs { maxAbs = abs(d) }
      counted += 1
    }
    let rms = sqrt(sumSq / Double(max(counted, 1)))
    print(
      String(
        format: "[impulse] aligned (lag=%d)  rms=%.3e  maxDiff=%.3e  N=%d",
        lag, rms, maxAbs, counted))
    // Bit-identical impulse response — kernel build, dot-product reduction,
    // and interp_cubic all match rubato exactly.
    XCTAssertEqual(
      rms, 0.0, "AsyncSinc impulse response should be bit-identical to rubato")
  }

  // MARK: - AsyncSinc veryFast / Fast / Balanced profiles

  func testVeryFast_Sine1kHz_44100to48000() throws {
    try runSincProfileComparison(
      mode: "sinc-veryfast", profile: .veryFast,
      label: "veryFast sine 1kHz")
  }

  func testFast_Sine1kHz_44100to48000() throws {
    try runSincProfileComparison(
      mode: "sinc-fast", profile: .fast,
      label: "fast sine 1kHz")
  }

  func testBalanced_Sine1kHz_44100to48000() throws {
    try runSincProfileComparison(
      mode: "sinc-balanced", profile: .balanced,
      label: "balanced sine 1kHz")
  }

  func testVeryFast_Impulse_44100to48000() throws {
    try runSincProfileImpulse(
      mode: "sinc-veryfast", profile: .veryFast, label: "veryFast impulse")
  }

  func testFast_Impulse_44100to48000() throws {
    try runSincProfileImpulse(
      mode: "sinc-fast", profile: .fast, label: "fast impulse")
  }

  func testBalanced_Impulse_44100to48000() throws {
    try runSincProfileImpulse(
      mode: "sinc-balanced", profile: .balanced, label: "balanced impulse")
  }

  /// Common driver for the non-Accurate sinc profiles. Each runs a 1 kHz
  /// sine through Swift's `AsyncSincResampler` and rubato's matching
  /// `new_sinc` configuration; they should agree to floating-point noise.
  private func runSincProfileComparison(
    mode: String, profile: ResamplerProfile, label: String
  ) throws {
    let nbrIn = 32 * Self.chunkSize
    var input = [Double](repeating: 0, count: nbrIn)
    let omega = 2.0 * .pi * 1000.0 / Double(Self.inRate)
    for i in 0..<nbrIn { input[i] = sin(omega * Double(i)) }

    let inPath = "/tmp/cdsp_\(mode)_sine_in.raw"
    let refPath = "/tmp/cdsp_\(mode)_sine_ref.raw"
    try writeRaw(input, to: inPath)
    guard try runRubato(mode: mode, input: inPath, output: refPath) else {
      throw XCTSkip("rubato binary missing")
    }
    let ref = try readRaw(from: refPath)
    let swiftOut = runSwiftResampler(
      AsyncSincResampler(
        channels: 1, inputRate: Self.inRate, outputRate: Self.outRate,
        profile: profile, chunkSize: Self.chunkSize),
      input: input)

    XCTAssertGreaterThan(ref.count, 5000)
    XCTAssertGreaterThan(swiftOut.count, 5000)

    let result = findBestAlignment(
      swift: swiftOut, ref: ref, skip: 4000, length: 16384, maxLag: 8)
    print(
      String(
        format: "[%@] swift=%d ref=%d  bestLag=%d  rms=%.3e  maxDiff=%.3e",
        label, swiftOut.count, ref.count, result.lag, result.rms, result.maxDiff))
    XCTAssertEqual(result.lag, 0, "[\(label)] should align at lag 0")
    // Tolerance is profile-dependent: linear/quadratic interpolation in the
    // veryFast/Fast/Balanced profiles is bit-identical to rubato (poly part
    // has no libm calls). The whole expression collapses to ~1 ULP from
    // libm cos in the kernel build.
    XCTAssertLessThan(
      result.rms, 1e-15,
      "[\(label)] rms \(result.rms) exceeds expected ~ULP noise floor")
  }

  /// Impulse-response comparison for non-Accurate profiles. The peak index
  /// must match exactly and the RMS over a window around the peak should be
  /// at machine epsilon.
  private func runSincProfileImpulse(
    mode: String, profile: ResamplerProfile, label: String
  ) throws {
    let nbrIn = 32 * Self.chunkSize
    var input = [Double](repeating: 0, count: nbrIn)
    input[1000] = 1.0

    let inPath = "/tmp/cdsp_\(mode)_imp_in.raw"
    let refPath = "/tmp/cdsp_\(mode)_imp_ref.raw"
    try writeRaw(input, to: inPath)
    guard try runRubato(mode: mode, input: inPath, output: refPath) else {
      throw XCTSkip("rubato binary missing")
    }
    let ref = try readRaw(from: refPath)
    let swiftOut = runSwiftResampler(
      AsyncSincResampler(
        channels: 1, inputRate: Self.inRate, outputRate: Self.outRate,
        profile: profile, chunkSize: Self.chunkSize),
      input: input)

    let swiftPeak = absPeak(Array(swiftOut.prefix(8000)))
    let refPeak = absPeak(Array(ref.prefix(8000)))
    print(
      String(
        format: "[%@] swiftPeak=%d (%.6f) refPeak=%d (%.6f)",
        label, swiftPeak.idx, swiftPeak.val, refPeak.idx, refPeak.val))
    XCTAssertEqual(swiftPeak.idx, refPeak.idx, "[\(label)] peak index mismatch")

    let win = 256
    let lag = swiftPeak.idx - refPeak.idx
    var sumSq = 0.0
    var counted = 0
    for k in -win...win {
      let r = refPeak.idx + k
      let s = r + lag
      if r < 0 || s < 0 || r >= ref.count || s >= swiftOut.count { continue }
      let d = swiftOut[s] - ref[r]
      sumSq += d * d
      counted += 1
    }
    let rms = sqrt(sumSq / Double(max(counted, 1)))
    print(String(format: "[%@] aligned rms=%.3e N=%d", label, rms, counted))
    XCTAssertLessThan(
      rms, 1e-14,
      "[\(label)] impulse rms \(rms) exceeds expected FP-noise floor")
  }

  // MARK: - AsyncPoly cubic vs rubato poly-cubic

  func testAsyncPolyCubic_Sine1kHz_44100to48000() throws {
    try runPolyComparison(mode: "poly-cubic", interp: .cubic, label: "poly-cubic sine 1kHz")
  }

  func testAsyncPolyCubic_Impulse_44100to48000() throws {
    try runPolyImpulse(mode: "poly-cubic", interp: .cubic, label: "poly-cubic impulse")
  }

  func testAsyncPolySeptic_Sine1kHz_44100to48000() throws {
    try runPolyComparison(mode: "poly-septic", interp: .septic, label: "poly-septic sine 1kHz")
  }

  func testAsyncPolySeptic_Impulse_44100to48000() throws {
    try runPolyImpulse(mode: "poly-septic", interp: .septic, label: "poly-septic impulse")
  }

  // MARK: - AsyncPoly linear & quintic vs rubato

  func testAsyncPolyLinear_Sine1kHz_44100to48000() throws {
    try runPolyComparison(mode: "poly-linear", interp: .linear, label: "poly-linear sine 1kHz")
  }

  func testAsyncPolyLinear_Impulse_44100to48000() throws {
    try runPolyImpulse(mode: "poly-linear", interp: .linear, label: "poly-linear impulse")
  }

  func testAsyncPolyQuintic_Sine1kHz_44100to48000() throws {
    try runPolyComparison(mode: "poly-quintic", interp: .quintic, label: "poly-quintic sine 1kHz")
  }

  func testAsyncPolyQuintic_Impulse_44100to48000() throws {
    try runPolyImpulse(mode: "poly-quintic", interp: .quintic, label: "poly-quintic impulse")
  }

  private func runPolyComparison(
    mode: String, interp: PolyInterpolation, label: String
  ) throws {
    let nbrIn = 32 * Self.chunkSize
    var input = [Double](repeating: 0, count: nbrIn)
    let omega = 2.0 * .pi * 1000.0 / Double(Self.inRate)
    for i in 0..<nbrIn { input[i] = sin(omega * Double(i)) }

    let inPath = "/tmp/cdsp_\(mode)_sine_in.raw"
    let refPath = "/tmp/cdsp_\(mode)_sine_ref.raw"
    try writeRaw(input, to: inPath)
    guard try runRubato(mode: mode, input: inPath, output: refPath) else {
      throw XCTSkip("rubato binary missing")
    }
    let ref = try readRaw(from: refPath)
    let swiftOut = runPoly(interp, input: input)

    XCTAssertGreaterThan(ref.count, 5000)
    XCTAssertGreaterThan(swiftOut.count, 5000)

    // AsyncPoly now matches rubato's `Async::new_poly` algorithm exactly —
    // same `last_index`, same buffer shift, same Newton-form polynomial.
    let result = findBestAlignment(
      swift: swiftOut, ref: ref, skip: 1024, length: 16384, maxLag: 8)
    print(
      String(
        format:
          "[%@] swift=%d ref=%d  bestLag=%d  rms=%.3e  maxDiff=%.3e",
        label, swiftOut.count, ref.count, result.lag, result.rms, result.maxDiff))
    XCTAssertEqual(result.lag, 0, "[\(label)] AsyncPoly should align with rubato at lag 0")
    // AsyncPoly is bit-identical: same buffer layout, same Newton-form
    // polynomial, no FFT or kernel build to introduce libm rounding.
    XCTAssertLessThan(
      result.rms, 1e-15, "[\(label)] AsyncPoly should match rubato within FP noise")
  }

  private func runPolyImpulse(
    mode: String, interp: PolyInterpolation, label: String
  ) throws {
    let nbrIn = 32 * Self.chunkSize
    var input = [Double](repeating: 0, count: nbrIn)
    input[1000] = 1.0

    let inPath = "/tmp/cdsp_\(mode)_imp_in.raw"
    let refPath = "/tmp/cdsp_\(mode)_imp_ref.raw"
    try writeRaw(input, to: inPath)
    guard try runRubato(mode: mode, input: inPath, output: refPath) else {
      throw XCTSkip("rubato binary missing")
    }
    let ref = try readRaw(from: refPath)
    let swiftOut = runPoly(interp, input: input)

    let swiftPeak = absPeak(Array(swiftOut.prefix(4096)))
    let refPeak = absPeak(Array(ref.prefix(4096)))
    let lag = swiftPeak.idx - refPeak.idx
    print(
      String(
        format: "[%@] swiftPeak=%d (%.6f) refPeak=%d (%.6f) lag=%d",
        label, swiftPeak.idx, swiftPeak.val, refPeak.idx, refPeak.val, lag))

    // Now bit-identical to rubato — same Newton-form polynomial, same
    // buffer shift. Peak frame index, peak value, and surrounding window
    // all match exactly.
    XCTAssertEqual(swiftPeak.idx, refPeak.idx, "[\(label)] peak index should match rubato")

    let win = 32
    var sumSq = 0.0
    var maxAbs = 0.0
    var counted = 0
    for k in -win...win {
      let r = refPeak.idx + k
      let s = r + lag
      if r < 0 || s < 0 || r >= ref.count || s >= swiftOut.count { continue }
      let d = swiftOut[s] - ref[r]
      sumSq += d * d
      if abs(d) > maxAbs { maxAbs = abs(d) }
      counted += 1
    }
    let rms = sqrt(sumSq / Double(max(counted, 1)))
    print(
      String(
        format: "[%@] aligned rms=%.3e maxDiff=%.3e N=%d",
        label, rms, maxAbs, counted))
    XCTAssertLessThan(
      maxAbs, 1e-15, "[\(label)] AsyncPoly impulse should match rubato within FP noise")
  }

  // MARK: - Synchronous vs rubato fft

  /// Compares Swift's `SynchronousResampler` against rubato's `Fft` resampler
  /// in `FixedSync::Both, sub_chunks=1` mode. With Bluestein FFT under the
  /// hood the Swift path matches rubato's overlap-save FFT resampler bit-for
  /// -bit modulo floating-point noise.
  func testSynchronous_Sine1kHz_vs_RubatoFft() throws {
    let nbrIn = 32 * Self.chunkSize
    var input = [Double](repeating: 0, count: nbrIn)
    let omega = 2.0 * .pi * 1000.0 / Double(Self.inRate)
    for i in 0..<nbrIn { input[i] = sin(omega * Double(i)) }

    let inPath = "/tmp/cdsp_fft_sine_in.raw"
    let refPath = "/tmp/cdsp_fft_sine_ref.raw"
    try writeRaw(input, to: inPath)
    guard try runRubato(mode: "fft", input: inPath, output: refPath) else {
      throw XCTSkip("rubato binary missing")
    }
    let ref = try readRaw(from: refPath)
    let swiftOut = runSync(input: input)

    XCTAssertGreaterThan(ref.count, 5000)
    XCTAssertGreaterThan(swiftOut.count, 5000)

    let result = findBestAlignment(
      swift: swiftOut, ref: ref, skip: 4000, length: 16384, maxLag: 4)
    print(
      String(
        format:
          "[fft sine 1kHz] swift=%d ref=%d  bestLag=%d  rms=%.3e  maxDiff=%.3e",
        swiftOut.count, ref.count, result.lag, result.rms, result.maxDiff))
    XCTAssertEqual(result.lag, 0, "FFT resampler should align with rubato (no lag)")
    // FFT path can't be bit-identical: vDSP-backed Bluestein FFT and rubato's
    // realfft (RustFFT mixed-radix) reorder operations differently. Max diff
    // stays within ~10 ULPs at amplitude 1.0.
    XCTAssertLessThan(result.rms, 5e-15, "FFT resampler should match rubato within FP noise")
  }
}
