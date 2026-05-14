// Single coherent comparison across the in-tree resamplers and (when
// the rust harness is built) the rubato reference. Every cell of the
// 4-implementation × 9-rate-pair matrix reports five metrics:
//
//   * Aliasing rejection — only meaningful for downsampling pairs.
//     A tone halfway between output Nyquist and input Nyquist sits
//     in the kernel's stopband; the residual RMS in the output
//     measures how much of it leaks through.
//   * Passband peak deviation — pure tones at fractions of
//     `min(in_nyq, out_nyq)`; reports the max |dB deviation| from
//     unity gain.
//   * Linear-phase / impulse symmetry — for integer 2:1 ratios this
//     should hit FP noise; for non-integer ratios the impulse
//     response is sampled off-grid, so a large value here is by
//     construction, not a fault.
//   * Round-trip SINAD — 1 kHz sine through `i → o → i`; projects
//     the recovered signal onto sin/cos at 1 kHz and reports
//     `10·log10(signal² / residual²)`.
//   * Throughput — ns/output-frame and real-time factor (RTF) on a
//     64×1024-frame random workload. Compare ratios across columns
//     for relative speed.
//
// Output is four printed tables (one per metric) plus one
// throughput table. The harness asserts per-cell regression bounds
// for the in-tree `SynchronousResampler` only; Apple and rubato
// rows are reference points (printed but not asserted) since their
// behaviour is outside our control.
//
// All bounds are clamped to the values currently measured by this
// test on this machine — any quality or speed regression in
// `SynchronousResampler` trips the matching expectation.

import Foundation
import Testing

@testable import DSPAudio
@testable import DSPResampler

@Suite struct ResamplerComparisonMatrix {

  // MARK: - Configuration

  static let rateGrid: [(inRate: Int, outRate: Int, label: String)] = [
    (44100, 48000, "44.1→48k"),
    (48000, 44100, "48→44.1k"),
    (48000, 96000, "48→96k"),
    (96000, 48000, "96→48k"),
    (44100, 88200, "44.1→88.2k"),
    (88200, 44100, "88.2→44.1k"),
    (44100, 192000, "44.1→192k"),
    (192000, 44100, "192→44.1k"),
    (37000, 41000, "37k→41k"),
  ]

  static let chunkSize = 1024
  static let totalChunks = 64
  /// Floor on perf-bench iterations so timing has at least this many
  /// samples even on very fast resamplers. The actual loop stops as
  /// soon as it has both ≥ this many iters *and* ≥ `perfMinDuration`
  /// of timed work — so slow resamplers (e.g. Apple mastering at
  /// 44.1↔192k, ~40 ms/iter) don't push the suite from seconds to
  /// minutes.
  static let perfMinIters = 20
  static let perfMinDuration: Duration = .milliseconds(400)

  // MARK: - Cells

  private struct Cell {
    var aliasingDb: Double?
    var passbandDb: Double?
    var symmetry: Double?
    var sinadDb: Double?
    var nsPerOutFrame: Double?
    var rtfPerIter: Double?
  }

  // MARK: - Swift Synchronous regression bounds (clamped to measured)

  /// Per-rate-pair regression bounds for `SynchronousResampler`.
  /// Each bound sits right above (max-type) or below (min-type) the
  /// value this test produces today. A regression in cutoff design,
  /// kernel-build, or FFT scaling will trip the matching #expect.
  ///
  /// `aliasingMaxDb` is `nil` for upsampling pairs (no in-band
  /// aliasing target frequency). `symmetry` clamps right above the
  /// measured asymmetry — for integer 2:1 ratios this is FP noise
  /// (~1e-16), for non-integer ratios it is the by-construction
  /// off-grid value (0.2–0.9).
  private struct SwiftBounds {
    let aliasingMaxDb: Double?
    let passbandMaxDb: Double
    let symmetryMax: Double
    let sinadMinDb: Double
  }

  // Quality bounds sit right above (max-type) or below (min-type)
  // the value the test produces.
  private static let swiftBounds: [String: SwiftBounds] = [
    "44.1→48k": SwiftBounds(
      aliasingMaxDb: nil, passbandMaxDb: 2.0e-2, symmetryMax: 0.92, sinadMinDb: 208.0
    ),
    "48→44.1k": SwiftBounds(
      aliasingMaxDb: -185.0, passbandMaxDb: 5.0e-3, symmetryMax: 0.70, sinadMinDb: 204.0
    ),
    "48→96k": SwiftBounds(
      aliasingMaxDb: nil, passbandMaxDb: 1.0e-2, symmetryMax: 2.0e-15, sinadMinDb: 205.0
    ),
    "96→48k": SwiftBounds(
      aliasingMaxDb: -228.0, passbandMaxDb: 2.5e-2, symmetryMax: 2.0e-12, sinadMinDb: 205.0
    ),
    "44.1→88.2k": SwiftBounds(
      aliasingMaxDb: nil, passbandMaxDb: 1.0e-2, symmetryMax: 2.0e-15, sinadMinDb: 208.0
    ),
    "88.2→44.1k": SwiftBounds(
      aliasingMaxDb: -228.0, passbandMaxDb: 2.5e-2, symmetryMax: 2.0e-12, sinadMinDb: 204.0
    ),
    "44.1→192k": SwiftBounds(
      aliasingMaxDb: nil, passbandMaxDb: 2.0e-2, symmetryMax: 0.32, sinadMinDb: 200.0
    ),
    "192→44.1k": SwiftBounds(
      aliasingMaxDb: -230.0, passbandMaxDb: 1.0e-2, symmetryMax: 0.45, sinadMinDb: 199.0
    ),
    "37k→41k": SwiftBounds(
      aliasingMaxDb: nil, passbandMaxDb: 6.2e-3, symmetryMax: 0.21, sinadMinDb: 250.0
    ),
  ]

  // MARK: - Top-level test

  @Test func compareAcrossRateGrid() throws {
    let rubatoOK = RubatoHarness.resamplerCompareAvailable

    // Process all 9 rate pairs sequentially to ensure stable, isolated performance timing.
    var entries:
      [(
        index: Int, label: String,
        swift: Cell, mast: Cell, minPh: Cell, rubato: Cell?
      )] = []
    for (i, entry) in Self.rateGrid.enumerated() {
      let r = computeRowForRatePair(
        index: i, inRate: entry.inRate, outRate: entry.outRate, label: entry.label,
        rubatoOK: rubatoOK)
      entries.append(r)
    }

    let grid: [(label: String, swift: Cell, mast: Cell, minPh: Cell, rubato: Cell?)] =
      entries.map { ($0.label, $0.swift, $0.mast, $0.minPh, $0.rubato) }

    // -- Print tables. Rows that are "N/A" for every implementation
    // of a given metric are skipped automatically (e.g. upsampling
    // pairs in the aliasing table).
    printTable(
      grid: grid, title: "Round-trip SINAD (1 kHz sine)",
      metric: \.sinadDb, higherIsBetter: true, format: "%6.1f dB")
    printTable(
      grid: grid, title: "Aliasing rejection (mid-stopband tone)",
      metric: \.aliasingDb, higherIsBetter: false, format: "%7.1f dB")
    printTable(
      grid: grid, title: "Passband peak deviation",
      metric: \.passbandDb, higherIsBetter: false, format: "%7.4f dB")
    printTable(
      grid: grid, title: "Impulse-response asymmetry (rel to peak)",
      metric: \.symmetry, higherIsBetter: false, format: "%9.3e")
    printTable(
      grid: grid, title: "Throughput (ns/output-frame)",
      metric: \.nsPerOutFrame, higherIsBetter: false, format: "%8.1f")
    printTable(
      grid: grid, title: "Throughput (real-time factor per iteration)",
      metric: \.rtfPerIter, higherIsBetter: true, format: "%7.1fx")

    // -- Assert Swift Synchronous regressions
    for entry in grid {
      guard let bounds = Self.swiftBounds[entry.label] else { continue }
      let c = entry.swift
      if let target = bounds.aliasingMaxDb, let measured = c.aliasingDb {
        #expect(
          measured < target,
          "[\(entry.label)] Swift aliasing \(measured) dB shallower than target \(target) dB")
      }
      if let measured = c.passbandDb {
        #expect(
          measured < bounds.passbandMaxDb,
          "[\(entry.label)] Swift passband \(measured) dB > target \(bounds.passbandMaxDb) dB")
      }
      if let measured = c.symmetry {
        #expect(
          measured < bounds.symmetryMax,
          "[\(entry.label)] Swift impulse asymmetry \(measured) > target \(bounds.symmetryMax)")
      }
      if let measured = c.sinadDb {
        #expect(
          measured > bounds.sinadMinDb,
          "[\(entry.label)] Swift round-trip SINAD \(measured) dB < target \(bounds.sinadMinDb) dB")
      }
    }
  }

  // MARK: - Per-row driver (one rate-pair, all four implementations)

  /// Runs the 5 quality + 1 perf measurements for all four
  /// implementations at a single rate pair. Pure (no shared mutable
  /// state) so it can run on a `TaskGroup` worker.
  private func computeRowForRatePair(
    index: Int, inRate: Int, outRate: Int, label: String, rubatoOK: Bool
  ) -> (
    index: Int, label: String,
    swift: Cell, mast: Cell, minPh: Cell, rubato: Cell?
  ) {
    let swiftProcess: ProcessFn = { input, ir, or in
      let res = SynchronousResampler(
        channels: 1, inputRate: ir, outputRate: or, chunkSize: Self.chunkSize)
      return self.runResampler(res, input: input)
    }
    let appleMastProcess: ProcessFn = { input, ir, or in
      guard
        let res = try? AppleResampler(
          channels: 1, inputRate: ir, outputRate: or,
          quality: .max, complexity: .mastering, chunkSize: Self.chunkSize)
      else { return nil }
      return self.runResampler(res, input: input)
    }
    let appleMinPhProcess: ProcessFn = { input, ir, or in
      guard
        let res = try? AppleResampler(
          channels: 1, inputRate: ir, outputRate: or,
          quality: .max, complexity: .minimumPhase, chunkSize: Self.chunkSize)
      else { return nil }
      return self.runResampler(res, input: input)
    }
    let rubatoProcess: ProcessFn = { input, ir, or in
      guard rubatoOK else { return nil }
      return self.runRubatoFft(inRate: ir, outRate: or, input: input)
    }

    var swift = measureQualityCell(inRate: inRate, outRate: outRate, process: swiftProcess)
    var mast = measureQualityCell(inRate: inRate, outRate: outRate, process: appleMastProcess)
    var minPh = measureQualityCell(inRate: inRate, outRate: outRate, process: appleMinPhProcess)
    var rubato: Cell? =
      rubatoOK
      ? measureQualityCell(inRate: inRate, outRate: outRate, process: rubatoProcess)
      : nil

    if let perf = measureSwiftPerf(
      inRate: inRate, outRate: outRate,
      factory: {
        SynchronousResampler(
          channels: 1, inputRate: $0, outputRate: $1, chunkSize: Self.chunkSize)
      })
    {
      swift.nsPerOutFrame = perf.nsPerOutFrame
      swift.rtfPerIter = perf.rtfPerIter
    }
    if let perf = measureSwiftPerf(
      inRate: inRate, outRate: outRate,
      factory: { i, o in
        try AppleResampler(
          channels: 1, inputRate: i, outputRate: o,
          quality: .max, complexity: .mastering, chunkSize: Self.chunkSize)
      })
    {
      mast.nsPerOutFrame = perf.nsPerOutFrame
      mast.rtfPerIter = perf.rtfPerIter
    }
    if let perf = measureSwiftPerf(
      inRate: inRate, outRate: outRate,
      factory: { i, o in
        try AppleResampler(
          channels: 1, inputRate: i, outputRate: o,
          quality: .max, complexity: .minimumPhase, chunkSize: Self.chunkSize)
      })
    {
      minPh.nsPerOutFrame = perf.nsPerOutFrame
      minPh.rtfPerIter = perf.rtfPerIter
    }
    if rubatoOK, var r = rubato,
      let perf = measureRubatoPerf(inRate: inRate, outRate: outRate)
    {
      r.nsPerOutFrame = perf.nsPerOutFrame
      r.rtfPerIter = perf.rtfPerIter
      rubato = r
    }

    return (index, label, swift, mast, minPh, rubato)
  }

  // MARK: - Quality measurement (any `process` closure)

  private typealias ProcessFn = (
    _ input: [Double], _ inRate: Int, _ outRate: Int
  ) -> [Double]?

  /// Compute the four quality metrics for one (rate-pair, impl)
  /// cell. The `process` closure abstracts over in-tree resamplers
  /// vs the rubato harness so the same body runs both.
  private func measureQualityCell(
    inRate: Int, outRate: Int, process: ProcessFn
  ) -> Cell {
    var c = Cell()
    let cs = Self.chunkSize
    let nbrIn = Self.totalChunks * cs
    // Output-side skip: 4× input-chunk worth of output frames covers
    // any startup transient regardless of the resampler's internal
    // block size.
    let outSkip = max(1, 4 * cs * outRate / inRate)

    // Aliasing — only when downsampling.
    if outRate < inRate {
      let outNy = 0.5 * Double(outRate)
      let inNy = 0.5 * Double(inRate)
      let testFreq = 0.5 * (outNy + inNy)
      let signal = sine(rate: inRate, freq: testFreq, samples: nbrIn)
      if let out = process(signal, inRate, outRate), out.count > outSkip {
        c.aliasingDb = 20.0 * log10(rms(out[outSkip...]) / sqrt(0.5))
      }
    }

    // Passband — sweep at fractions of min-Nyquist, take peak |dev|.
    let minNy = 0.5 * Double(min(inRate, outRate))
    var devs: [Double] = []
    for fraction in [0.001, 0.05, 0.5, 0.7, 0.85] {
      let signal = sine(rate: inRate, freq: fraction * minNy, samples: nbrIn)
      if let out = process(signal, inRate, outRate), out.count > outSkip {
        devs.append(20.0 * log10(rms(out[outSkip...]) / sqrt(0.5)))
      }
    }
    if !devs.isEmpty {
      c.passbandDb = devs.map { abs($0) }.max() ?? 0
    }

    // Impulse — single-sample impulse, asymmetry around the peak.
    var impulseInput = [Double](repeating: 0, count: 32 * cs)
    impulseInput[16 * cs] = 1.0
    if let out = process(impulseInput, inRate, outRate) {
      c.symmetry = impulseAsymmetry(output: out)
    }

    // Round-trip SINAD — i → o → i with a 1 kHz sine.
    let signal = sine(rate: inRate, freq: 1000, samples: nbrIn)
    if let upOut = process(signal, inRate, outRate),
      let downOut = process(upOut, outRate, inRate)
    {
      let skip = 4 * cs
      let endIdx = min(skip + 8192, downOut.count)
      if endIdx > skip {
        c.sinadDb = sinadDb(signal: Array(downOut[skip..<endIdx]), rate: inRate)
      }
    }

    return c
  }

  // MARK: - Throughput measurement (in-tree factory)

  /// Time `iters` sweeps of `totalChunks` random-input chunks
  /// through an in-tree resampler. Returns ns-per-output-frame and
  /// per-iteration real-time factor. Identical setup to the
  /// cross-language harness so numbers match.
  private func measureSwiftPerf(
    inRate: Int, outRate: Int,
    factory: (Int, Int) throws -> AudioResampler
  ) -> (nsPerOutFrame: Double, rtfPerIter: Double)? {
    do {
      let resampler = try factory(inRate, outRate)
      let cs = resampler.chunkSize
      let chunkCount = Self.totalChunks
      let nbrIn = chunkCount * cs

      var rng = SystemRandomNumberGenerator()
      var input = [Double](repeating: 0, count: nbrIn)
      for i in 0..<nbrIn { input[i] = Double.random(in: -1.0...1.0, using: &rng) }

      var chunks: [AudioChunk] = []
      chunks.reserveCapacity(chunkCount)
      for c in 0..<chunkCount {
        let slice = Array(input[c * cs..<(c + 1) * cs])
        chunks.append(AudioChunk(waveforms: [slice], validFrames: cs))
      }

      var scratch = AudioChunk(
        waveforms: [[Double](repeating: 0, count: resampler.maxOutputFrames)],
        validFrames: 0)

      // Warm-up sweep.
      for c in chunks { try resampler.process(input: c, into: &scratch) }

      let start = ContinuousClock.now
      var outFrames = 0
      var iters = 0
      while iters < Self.perfMinIters || ContinuousClock.now - start < Self.perfMinDuration {
        for c in chunks {
          try resampler.process(input: c, into: &scratch)
          outFrames += scratch.validFrames
        }
        iters += 1
      }
      let elapsed = ContinuousClock.now - start
      let elapsedNs =
        Double(elapsed.components.seconds) * 1e9
        + Double(elapsed.components.attoseconds) * 1e-9
      let nsPerOutFrame = elapsedNs / Double(outFrames)
      let inSec = Double(nbrIn) / Double(inRate)
      let rtfPerIter = inSec / (elapsedNs * 1e-9 / Double(iters))
      return (nsPerOutFrame, rtfPerIter)
    } catch {
      return nil
    }
  }

  // MARK: - Throughput measurement (rubato harness)

  /// Drive the rubato harness with `--bench=N` and parse the
  /// `BENCH_*` tokens it emits on stderr. Returns `nil` on any
  /// harness failure (treated as a soft skip).
  private func measureRubatoPerf(inRate: Int, outRate: Int)
    -> (nsPerOutFrame: Double, rtfPerIter: Double)?
  {
    var rng = SystemRandomNumberGenerator()
    let nbrIn = Self.totalChunks * Self.chunkSize
    var input = [Double](repeating: 0, count: nbrIn)
    for i in 0..<nbrIn { input[i] = Double.random(in: -1.0...1.0, using: &rng) }
    let inPath = "/tmp/cdsp_matrix_perf_\(inRate)_\(outRate)_in.raw"
    let outPath = "/tmp/cdsp_matrix_perf_\(inRate)_\(outRate)_out.raw"
    do { try RubatoHarness.writeRaw(input, to: inPath) } catch { return nil }

    let bin = RubatoHarness.binaryPath(named: "cdsp_resampler_compare")
    let proc = Process()
    proc.executableURL = URL(fileURLWithPath: bin)
    proc.arguments = [
      "fft", inPath, outPath,
      String(inRate), String(outRate), String(Self.chunkSize),
      "--bench=\(Self.perfMinIters)",
    ]
    let stderr = Pipe()
    proc.standardError = stderr
    do { try proc.run() } catch { return nil }
    proc.waitUntilExit()
    guard proc.terminationStatus == 0 else { return nil }

    let stderrData = stderr.fileHandleForReading.readDataToEndOfFile()
    let stderrStr = String(data: stderrData, encoding: .utf8) ?? ""
    guard let bench = parseBench(stderrStr) else { return nil }
    let nsPerOutFrame = Double(bench.nsTotal) / Double(bench.outFramesPerIter * bench.iters)
    let inSec = Double(nbrIn) / Double(inRate)
    let rtfPerIter = inSec / (Double(bench.nsTotal) * 1e-9 / Double(bench.iters))
    return (nsPerOutFrame, rtfPerIter)
  }

  private struct BenchOutput {
    let nsTotal: UInt64
    let outFramesPerIter: Int
    let iters: Int
  }

  private func parseBench(_ s: String) -> BenchOutput? {
    var nsTotal: UInt64?
    var outFrames: Int?
    var iters: Int?
    for token in s.split(whereSeparator: { $0.isWhitespace }) {
      let parts = token.split(separator: "=", maxSplits: 1)
      guard parts.count == 2 else { continue }
      let k = String(parts[0])
      let v = String(parts[1])
      switch k {
      case "BENCH_NS_TOTAL": nsTotal = UInt64(v)
      case "BENCH_OUT_FRAMES_PER_ITER": outFrames = Int(v)
      case "BENCH_ITERS": iters = Int(v)
      default: continue
      }
    }
    guard let n = nsTotal, let o = outFrames, let i = iters else { return nil }
    return BenchOutput(nsTotal: n, outFramesPerIter: o, iters: i)
  }

  // MARK: - rubato harness file I/O

  private func runRubatoFft(inRate: Int, outRate: Int, input: [Double]) -> [Double]? {
    let tag = "\(inRate)_\(outRate)_\(input.count)"
    let inPath = "/tmp/cdsp_matrix_\(tag)_in.raw"
    let outPath = "/tmp/cdsp_matrix_\(tag)_out.raw"
    do {
      try RubatoHarness.writeRaw(input, to: inPath)
      let ok = try RubatoHarness.runResamplerCompare(
        mode: "fft", inputPath: inPath, outputPath: outPath,
        inRate: inRate, outRate: outRate, chunkSize: Self.chunkSize,
        noPartial: true)
      guard ok else { return nil }
      return try RubatoHarness.readRaw(from: outPath)
    } catch {
      return nil
    }
  }

  // MARK: - Common signal helpers

  private func sine(rate: Int, freq: Double, samples: Int) -> [Double] {
    let omega = 2.0 * .pi * freq / Double(rate)
    return (0..<samples).map { sin(omega * Double($0)) }
  }

  private func runResampler(_ resampler: AudioResampler, input: [Double]) -> [Double] {
    let cs = resampler.chunkSize
    var inChunk = AudioChunk(frames: cs, channels: 1)
    var outChunk = AudioChunk(frames: resampler.maxOutputFrames, channels: 1)
    inChunk.validFrames = cs
    var output: [Double] = []
    output.reserveCapacity(Int(Double(input.count) * resampler.ratio) + 64)
    var idx = 0
    while idx + cs <= input.count {
      for i in 0..<cs { inChunk[0][i] = input[idx + i] }
      try? resampler.process(input: inChunk, into: &outChunk)
      for i in 0..<outChunk.validFrames { output.append(outChunk[0][i]) }
      idx += cs
    }
    return output
  }

  private func rms<S: Sequence>(_ samples: S) -> Double where S.Element == Double {
    var sumSq = 0.0
    var count = 0
    for v in samples {
      sumSq += v * v
      count += 1
    }
    return count > 0 ? sqrt(sumSq / Double(count)) : 0
  }

  /// Asymmetry around the absolute-value peak of `output`, divided
  /// by the peak height. Returns `.nan` if no peak is found.
  private func impulseAsymmetry(output: [Double], window: Int = 256) -> Double {
    var peakIdx = 0
    var peakVal = 0.0
    for (i, v) in output.enumerated() where abs(v) > peakVal {
      peakIdx = i
      peakVal = abs(v)
    }
    let win = min(peakIdx, output.count - peakIdx - 16, window)
    if win < 1 || peakVal == 0 { return Double.nan }

    let kernelRadius = 12
    let count = output.count

    // Hann-windowed sinc interpolator for sub-sample evaluation
    func sincInterp(at t: Double) -> Double {
      let tInt = Int(floor(t))
      let frac = t - Double(tInt)
      if abs(frac) < 1e-9 {
        guard tInt >= 0 && tInt < count else { return 0 }
        return output[tInt]
      }
      var val = 0.0
      for m in -kernelRadius...kernelRadius {
        let n = tInt + m
        guard n >= 0 && n < count else { continue }
        let x = Double(m) - frac
        let sinc = sin(.pi * x) / (.pi * x)
        let w = 0.5 * (1.0 + cos(.pi * Double(m) / Double(kernelRadius)))
        val += output[n] * sinc * w
      }
      return val
    }

    // Scan sub-sample candidate offsets to locate the true continuous center of symmetry
    var minAsym = Double.infinity
    for step in -20...20 {
      let delta = Double(step) / 40.0
      let center = Double(peakIdx) + delta
      var maxAsym = 0.0
      for k in 1...win {
        let l = sincInterp(at: center - Double(k))
        let r = sincInterp(at: center + Double(k))
        let asym = abs(l - r)
        if asym > maxAsym { maxAsym = asym }
      }
      if maxAsym < minAsym { minAsym = maxAsym }
    }

    return minAsym / peakVal
  }

  /// Project `signal` onto sin/cos at `freq` (least squares), treat
  /// the residual as noise+distortion, return SINAD in dB.
  private func sinadDb(signal: [Double], rate: Int, freq: Double = 1000.0) -> Double {
    let n = signal.count
    guard n > 0 else { return 0 }
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
    guard abs(det) > 1e-12 else { return 0 }
    let I = (sumSs * sumC1 - sumCs * sumS1) / det
    let Q = (sumCc * sumS1 - sumCs * sumC1) / det
    var sumSqError = 0.0
    for t in 0..<n {
      let c = cos(omega * Double(t))
      let s = sin(omega * Double(t))
      let fitted = I * c + Q * s
      sumSqError += (signal[t] - fitted) * (signal[t] - fitted)
    }
    let signalPower = (I * I + Q * Q) / 2.0
    let noisePower = sumSqError / Double(n)
    guard noisePower > 0 else { return Double.infinity }
    return 10.0 * log10(signalPower / noisePower)
  }

  // MARK: - Table printing

  private func printTable(
    grid: [(label: String, swift: Cell, mast: Cell, minPh: Cell, rubato: Cell?)],
    title: String,
    metric: KeyPath<Cell, Double?>,
    higherIsBetter: Bool,
    format: String
  ) {
    let pairCol = "Pair".padding(toLength: 14, withPad: " ", startingAt: 0)
    let header = ["Swift Sync", "Apple Mast", "Apple MinPh", "rubato Fft"]
      .map { $0.padding(toLength: 14, withPad: " ", startingAt: 0) }
      .joined(separator: " ")
    let directionStr = higherIsBetter ? "higher is better" : "lower is better"
    print("=== \(title) (\(directionStr)) ===")
    print("\(pairCol) \(header)")
    for row in grid {
      let values: [Double?] = [
        row.swift[keyPath: metric],
        row.mast[keyPath: metric],
        row.minPh[keyPath: metric],
        row.rubato?[keyPath: metric],
      ]
      // Skip rows where the metric is N/A for every implementation
      // (e.g. aliasing table for upsampling pairs).
      if values.allSatisfy({ $0 == nil || !$0!.isFinite }) { continue }

      let finiteValues = values.compactMap { $0 }.filter { $0.isFinite }
      let bestValue = higherIsBetter ? finiteValues.max() : finiteValues.min()

      let cells = values.map { val -> String in
        guard let v = val, v.isFinite else {
          return " N/A".padding(toLength: 14, withPad: " ", startingAt: 0)
        }
        let trimmed = String(format: format, v).trimmingCharacters(in: .whitespaces)
        let isBest = (bestValue != nil) && (v == bestValue!)
        let cellStr = isBest ? "(\(trimmed))" : " \(trimmed)"
        return cellStr.padding(toLength: 14, withPad: " ", startingAt: 0)
      }.joined(separator: " ")
      print("\(row.label.padding(toLength: 14, withPad: " ", startingAt: 0)) \(cells)")
    }
  }
}
