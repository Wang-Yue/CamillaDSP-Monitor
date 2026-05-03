// Rate-matrix correctness tests. Spreads the four resamplers across a
// representative grid of audio sample-rate ratios and chunk sizes, comparing
// each Swift output to the corresponding rubato output.
//
// `ResamplerComparisonTests` already covers the 44.1 → 48 kHz path in depth;
// this suite is about exercising the *factorisations* and *ratios* the
// production code will actually see in the wild:
//
//   ratio < 1 and ratio > 1
//   integer (2×, 4×) and rational (320/441, 160/147, 320/49) ratios
//   chunk sizes that round up vs. land exactly on the FFT block size
//   prime factors covering 2, 3, 5, 7
//   stereo (multi-channel) for resamplers with per-channel state
//
// Each test invokes the rubato harness via `ResamplerComparisonTests`'s
// `harnessBinary(named:)` helper so the binary lookup stays consistent.

import Foundation
import XCTest

@testable import CamillaDSPLib

final class ResamplerRateMatrixTests: XCTestCase {

  // MARK: - Test matrix

  /// (inRate, outRate, chunkSize, label). Picked so MixedRadixFFT (2/3/5/7
  /// factors) is exercised in every direction and on both common audio chunk
  /// sizes. 44.1 ↔ 192 kHz is the worst case for FFT factorisation.
  private static let rateGrid: [(Int, Int, Int, String)] = [
    (44100, 48000, 1024, "44.1→48k cs=1024"),
    (44100, 48000, 2048, "44.1→48k cs=2048"),
    (48000, 44100, 1024, "48→44.1k cs=1024"),
    (48000, 96000, 1024, "48→96k cs=1024"),
    (96000, 48000, 1024, "96→48k cs=1024"),
    (44100, 88200, 1024, "44.1→88.2k cs=1024"),
    (88200, 44100, 1024, "88.2→44.1k cs=1024"),
    (44100, 192000, 1024, "44.1→192k cs=1024"),
    (192000, 44100, 1024, "192→44.1k cs=1024"),
    (11000, 13000, 1024, "11→13k cs=1024"),  // Exotic ratio forcing Bluestein fallback
  ]

  // MARK: - I/O

  private func writeRaw(_ data: [Double], to path: String) throws {
    let buf = data.withUnsafeBufferPointer { Data(buffer: $0) }
    try buf.write(to: URL(fileURLWithPath: path))
  }

  private func readRaw(from path: String) throws -> [Double] {
    let data = try Data(contentsOf: URL(fileURLWithPath: path))
    let count = data.count / MemoryLayout<Double>.stride
    return data.withUnsafeBytes { raw in
      let p = raw.bindMemory(to: Double.self)
      return Array(UnsafeBufferPointer(start: p.baseAddress, count: count))
    }
  }

  private func runRubato(
    mode: String, input: String, output: String,
    inRate: Int, outRate: Int, chunkSize: Int
  ) throws -> Bool {
    let bin = ResamplerComparisonTests.harnessBinary(named: "cdsp_resampler_compare")
    guard FileManager.default.isExecutableFile(atPath: bin) else { return false }
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
      XCTFail("rubato harness failed: \(err)")
      return false
    }
    return true
  }

  // MARK: - Driver

  /// Run one Swift resampler over a flat input array, slicing by the
  /// resampler's actual `chunkSize` (Synchronous rounds it up to the
  /// nearest valid FFT block size).
  private func runSwift(_ resampler: AudioResampler, input: [Double]) -> [Double] {
    let cs = resampler.chunkSize
    var output: [Double] = []
    output.reserveCapacity(Int(Double(input.count) * resampler.ratio) + 64)
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

  /// Generate a 1 kHz sine of length `n` at sample rate `rate`. We use a
  /// 1 kHz tone because rubato's anti-aliasing kernel passes it cleanly at
  /// every rate in the grid — this lets us assert tight tolerances without
  /// fighting the filter response near Nyquist.
  private func makeSine(n: Int, rate: Int, freq: Double = 1000.0) -> [Double] {
    let omega = 2.0 * .pi * freq / Double(rate)
    return (0..<n).map { sin(omega * Double($0)) }
  }

  /// Find the lag in [-maxLag, +maxLag] (Swift relative to ref) that
  /// minimises RMS difference over `length` samples starting at `skip`.
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
      if counted > 0 {
        let rms = sqrt(sumSq / Double(counted))
        if rms < bestRms {
          bestRms = rms
          bestMax = maxAbs
          bestLag = lag
        }
      }
    }
    return (bestLag, bestRms, bestMax)
  }

  /// Run one (mode, swiftFactory) combination over the entire rate grid.
  /// `swiftFactory` builds the matching Swift resampler for given rates
  /// and chunk size; `mode` is the rubato harness flag.
  private func runMatrix(
    mode: String,
    label: String,
    rmsTolerance: Double,
    swiftFactory: (Int, Int, Int) -> AudioResampler
  ) throws {
    let bin = ResamplerComparisonTests.harnessBinary(named: "cdsp_resampler_compare")
    guard FileManager.default.isExecutableFile(atPath: bin) else {
      throw XCTSkip("rubato harness missing")
    }

    for (inRate, outRate, chunkSize, ratLabel) in Self.rateGrid {
      // 32 chunks ≈ 0.7 s at 44.1 kHz — long enough that the resampler's
      // transient state has fully bled out before we measure.
      let nbrIn = 32 * chunkSize
      let input = makeSine(n: nbrIn, rate: inRate)
      let inPath = "/tmp/cdsp_matrix_\(mode)_\(inRate)_\(outRate)_\(chunkSize)_in.raw"
      let refPath = "/tmp/cdsp_matrix_\(mode)_\(inRate)_\(outRate)_\(chunkSize)_ref.raw"
      try writeRaw(input, to: inPath)
      let ok = try runRubato(
        mode: mode, input: inPath, output: refPath,
        inRate: inRate, outRate: outRate, chunkSize: chunkSize)
      XCTAssertTrue(ok, "[\(label) \(ratLabel)] rubato harness failed")
      let ref = try readRaw(from: refPath)

      let resampler = swiftFactory(inRate, outRate, chunkSize)
      let swiftOut = runSwift(resampler, input: input)

      // Need enough overlap to align over a meaningful window.
      let windowLen = min(8192, min(ref.count, swiftOut.count) - 4096)
      XCTAssertGreaterThan(
        windowLen, 1024,
        "[\(label) \(ratLabel)] not enough output to compare (\(swiftOut.count) vs \(ref.count))")

      let result = findBestAlignment(
        swift: swiftOut, ref: ref, skip: 2048, length: windowLen, maxLag: 8)

      // Useful diagnostic for tuning tolerances if the test is flaky.
      print(
        String(
          format: "[%@ %@] swift=%d ref=%d lag=%d rms=%.3e max=%.3e",
          label, ratLabel, swiftOut.count, ref.count, result.lag, result.rms, result.maxDiff))

      XCTAssertEqual(result.lag, 0, "[\(label) \(ratLabel)] lag should be 0")
      XCTAssertLessThan(
        result.rms, rmsTolerance,
        "[\(label) \(ratLabel)] rms \(result.rms) exceeds tolerance \(rmsTolerance)")
    }
  }

  // MARK: - Tests, one per resampler type

  func testMatrix_AsyncSincAccurate() throws {
    // AsyncSinc has libm-cos in the kernel build, so per-bin error is at
    // best ~ε. Across 8192 samples at amplitude ≤ 1 we comfortably stay
    // under 1e-13.
    try runMatrix(
      mode: "sinc-accurate",
      label: "AsyncSinc Accurate",
      rmsTolerance: 1e-13,
      swiftFactory: { inR, outR, cs in
        AsyncSincResampler(
          channels: 1, inputRate: inR, outputRate: outR,
          profile: .accurate, chunkSize: cs)
      })
  }

  func testMatrix_AsyncPolyCubic() throws {
    // AsyncPoly is bit-identical to rubato in single-rate experiments —
    // tightening to literal zero is the strongest signal for regressions.
    try runMatrix(
      mode: "poly-cubic",
      label: "AsyncPoly Cubic",
      rmsTolerance: 1e-15,
      swiftFactory: { inR, outR, cs in
        AsyncPolyResampler(
          channels: 1, inputRate: inR, outputRate: outR,
          interpolation: .cubic, chunkSize: cs)
      })
  }

  func testMatrix_AsyncPolySeptic() throws {
    try runMatrix(
      mode: "poly-septic",
      label: "AsyncPoly Septic",
      rmsTolerance: 1e-15,
      swiftFactory: { inR, outR, cs in
        AsyncPolyResampler(
          channels: 1, inputRate: inR, outputRate: outR,
          interpolation: .septic, chunkSize: cs)
      })
  }

  func testMatrix_SynchronousFFT() throws {
    // Synchronous FFT is bounded by Bluestein-vs-RustFFT op reordering
    // — typically ~10 ULPs at amplitude 1.0, so ~5e-15 RMS over 8 k samples.
    try runMatrix(
      mode: "fft",
      label: "Synchronous FFT",
      rmsTolerance: 5e-14,
      swiftFactory: { inR, outR, cs in
        SynchronousResampler(
          channels: 1, inputRate: inR, outputRate: outR, chunkSize: cs)
      })
  }

  // MARK: - Multi-channel

  /// Verify that a stereo (2-channel) resampler produces the same output
  /// per channel as 1-channel runs would. This exercises the per-channel
  /// state (overlap buffers in Synchronous, input-buffer per channel in
  /// AsyncSinc/Poly).
  func testStereo_MatchesPerChannelMono_Synchronous() throws {
    let inRate = 44100
    let outRate = 48000
    let chunkSize = 1024
    let nbrIn = 32 * chunkSize
    let left = makeSine(n: nbrIn, rate: inRate, freq: 1000.0)
    let right = makeSine(n: nbrIn, rate: inRate, freq: 1500.0)

    let stereo = SynchronousResampler(
      channels: 2, inputRate: inRate, outputRate: outRate, chunkSize: chunkSize)
    let monoL = SynchronousResampler(
      channels: 1, inputRate: inRate, outputRate: outRate, chunkSize: chunkSize)
    let monoR = SynchronousResampler(
      channels: 1, inputRate: inRate, outputRate: outRate, chunkSize: chunkSize)
    let cs = stereo.chunkSize  // possibly rounded up

    var stereoOutL: [Double] = []
    var stereoOutR: [Double] = []
    var monoOutL: [Double] = []
    var monoOutR: [Double] = []
    var idx = 0
    while idx + cs <= nbrIn {
      let l = Array(left[idx..<idx + cs])
      let r = Array(right[idx..<idx + cs])
      let stChunk = AudioChunk(waveforms: [l, r], validFrames: cs)
      let stOut = try! stereo.process(chunk: stChunk)
      stereoOutL.append(contentsOf: stOut.waveforms[0][0..<stOut.validFrames])
      stereoOutR.append(contentsOf: stOut.waveforms[1][0..<stOut.validFrames])

      let lChunk = AudioChunk(waveforms: [l], validFrames: cs)
      let rChunk = AudioChunk(waveforms: [r], validFrames: cs)
      let lOut = try! monoL.process(chunk: lChunk)
      let rOut = try! monoR.process(chunk: rChunk)
      monoOutL.append(contentsOf: lOut.waveforms[0][0..<lOut.validFrames])
      monoOutR.append(contentsOf: rOut.waveforms[0][0..<rOut.validFrames])
      idx += cs
    }
    XCTAssertEqual(stereoOutL.count, monoOutL.count)
    XCTAssertEqual(stereoOutR.count, monoOutR.count)
    var maxL = 0.0
    var maxR = 0.0
    for i in 0..<stereoOutL.count {
      maxL = max(maxL, abs(stereoOutL[i] - monoOutL[i]))
      maxR = max(maxR, abs(stereoOutR[i] - monoOutR[i]))
    }
    // The per-channel state is independent, so stereo[ch] should equal
    // mono[ch] bit-for-bit.
    XCTAssertEqual(maxL, 0.0, "stereo L diverges from mono L by \(maxL)")
    XCTAssertEqual(maxR, 0.0, "stereo R diverges from mono R by \(maxR)")
  }

  // MARK: - Round-trip (Swift-only, no harness)

  /// Driver for the round-trip test grid: send a 1 kHz sine through
  /// `up (44.1 → 48) → down (48 → 44.1)` using the same resampler family
  /// in both directions, then search for the best lag alignment between
  /// the recovered signal and the original. Each resampler instance has
  /// its own group delay (`sincLen` for the sinc profiles,
  /// `nbrPoints/2` for poly, the FFT-block delay for Synchronous), so
  /// the output is always offset; `findBestAlignment` finds it.
  ///
  /// `rmsTolerance` is generous on purpose — these tests aren't
  /// quality benchmarks, they catch *catastrophic* regressions (NaN
  /// blow-ups, sign-flipped twiddles, wrong permutation, etc.) where
  /// RMS would shoot above ~1.0. The print line is the part you read
  /// to compare relative quality across profiles.
  private func calculateSINAD(signal: [Double], rate: Int, freq: Double = 1000.0) -> Double {
    let n = signal.count
    guard n > 0 else { return 0.0 }

    let omega = 2.0 * .pi * freq / Double(rate)

    var sumC1 = 0.0
    var sumS1 = 0.0
    var sumCc = 0.0
    var sumSs = 0.0
    var sumCs = 0.0

    for t in 0..<n {
      let c = cos(omega * Double(t))
      let s = sin(omega * Double(t))
      let y = signal[t]

      sumC1 += y * c
      sumS1 += y * s
      sumCc += c * c
      sumSs += s * s
      sumCs += c * s
    }

    let det = sumCc * sumSs - sumCs * sumCs
    guard abs(det) > 1e-12 else { return 0.0 }

    let I = (sumSs * sumC1 - sumCs * sumS1) / det
    let Q = (sumCc * sumS1 - sumCs * sumC1) / det

    var sumSqError = 0.0
    for t in 0..<n {
      let c = cos(omega * Double(t))
      let s = sin(omega * Double(t))
      let fitted = I * c + Q * s
      let error = signal[t] - fitted
      sumSqError += error * error
    }

    let signalPower = (I * I + Q * Q) / 2.0
    let noisePower = sumSqError / Double(n)

    guard noisePower > 0.0 else { return 140.0 }  // Cap at 140 dB if noiseless

    return 10.0 * log10(signalPower / noisePower)
  }

  private func runRoundTrip(
    label: String,
    rmsTolerance: Double,
    factory: (Int, Int, Int) -> AudioResampler
  ) {
    let inRate = 44100
    let midRate = 48000
    let chunkSize = 1024
    let nbrIn = 64 * chunkSize  // long enough that alignment search has slack

    let signal = makeSine(n: nbrIn, rate: inRate, freq: 1000.0)
    let up = factory(inRate, midRate, chunkSize)
    let upSignal = runSwift(up, input: signal)
    let down = factory(midRate, inRate, chunkSize)
    let downSignal = runSwift(down, input: upSignal)

    // Wide lag window — combined group delay can be ~512 samples for the
    // accurate sinc, much smaller for poly, irregular for Synchronous FFT.
    let result = findBestAlignment(
      swift: downSignal, ref: signal, skip: 4096, length: 8192, maxLag: 1024)

    let sinad = calculateSINAD(signal: Array(downSignal[4096..<4096 + 8192]), rate: inRate)

    print(
      String(
        format: "[round-trip 44.1↔48↔44.1 %@] lag=%d rms=%.3e max=%.3e sinad=%.1f dB",
        label, result.lag, result.rms, result.maxDiff, sinad))
    XCTAssertLessThan(
      result.rms, rmsTolerance,
      "[\(label)] round-trip RMS \(result.rms) suggests kernel/numeric regression")
  }

  // AsyncSinc — four profiles. Tolerances reflect the kernel quality:
  // accurate ≈ -50 dB, balanced ≈ -40 dB, fast/veryFast progressively
  // worse on a 1 kHz tone (still fine for catching regressions).
  func testRoundTrip_AsyncSinc_Accurate() throws {
    runRoundTrip(label: "AsyncSinc Accurate", rmsTolerance: 1e-2) { i, o, cs in
      AsyncSincResampler(
        channels: 1, inputRate: i, outputRate: o, profile: .accurate, chunkSize: cs)
    }
  }
  func testRoundTrip_AsyncSinc_Balanced() throws {
    runRoundTrip(label: "AsyncSinc Balanced", rmsTolerance: 1e-2) { i, o, cs in
      AsyncSincResampler(
        channels: 1, inputRate: i, outputRate: o, profile: .balanced, chunkSize: cs)
    }
  }
  func testRoundTrip_AsyncSinc_Fast() throws {
    runRoundTrip(label: "AsyncSinc Fast", rmsTolerance: 1e-1) { i, o, cs in
      AsyncSincResampler(
        channels: 1, inputRate: i, outputRate: o, profile: .fast, chunkSize: cs)
    }
  }
  func testRoundTrip_AsyncSinc_VeryFast() throws {
    runRoundTrip(label: "AsyncSinc VeryFast", rmsTolerance: 1e-1) { i, o, cs in
      AsyncSincResampler(
        channels: 1, inputRate: i, outputRate: o, profile: .veryFast, chunkSize: cs)
    }
  }

  // AsyncPoly — four interpolation orders. Linear is poor (no anti-
  // aliasing, bare 2-tap interpolation), so the round-trip RMS ends up
  // close to the signal amplitude — the assertion is mostly a NaN guard.
  func testRoundTrip_AsyncPoly_Linear() throws {
    runRoundTrip(label: "AsyncPoly Linear", rmsTolerance: 1.0) { i, o, cs in
      AsyncPolyResampler(
        channels: 1, inputRate: i, outputRate: o, interpolation: .linear, chunkSize: cs)
    }
  }
  func testRoundTrip_AsyncPoly_Cubic() throws {
    runRoundTrip(label: "AsyncPoly Cubic", rmsTolerance: 1.0) { i, o, cs in
      AsyncPolyResampler(
        channels: 1, inputRate: i, outputRate: o, interpolation: .cubic, chunkSize: cs)
    }
  }
  func testRoundTrip_AsyncPoly_Quintic() throws {
    runRoundTrip(label: "AsyncPoly Quintic", rmsTolerance: 1.0) { i, o, cs in
      AsyncPolyResampler(
        channels: 1, inputRate: i, outputRate: o, interpolation: .quintic, chunkSize: cs)
    }
  }
  func testRoundTrip_AsyncPoly_Septic() throws {
    runRoundTrip(label: "AsyncPoly Septic", rmsTolerance: 1.0) { i, o, cs in
      AsyncPolyResampler(
        channels: 1, inputRate: i, outputRate: o, interpolation: .septic, chunkSize: cs)
    }
  }

  // Synchronous FFT — strict tolerance because rubato's overlap-save
  // path is essentially perfect for in-band tones.
  func testRoundTrip_SynchronousFFT() throws {
    runRoundTrip(label: "Synchronous FFT", rmsTolerance: 1e-2) { i, o, cs in
      SynchronousResampler(
        channels: 1, inputRate: i, outputRate: o, chunkSize: cs)
    }
  }
}
