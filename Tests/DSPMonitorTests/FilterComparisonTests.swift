// Comparison tests for Swift filter implementations against camilladsp's
// reference. Drives the `cdsp_filter_compare` Rust harness in
// `~/cdsp_filter_compare`.
//
// To use: build the harness once with
//   cd ~/cdsp_filter_compare && cargo build --release
// Tests are skipped (with a warning) when the binary is missing.

import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters
@testable import DSPMixer

@Suite struct FilterComparisonTests {

  static let chunkSize = 1024
  static let sampleRate = 48000
  static let nbrFrames = 16 * chunkSize  // 16384 — long enough that a stable IIR
  // settles to its steady-state response

  static var harnessBinary: String {
    if let env = ProcessInfo.processInfo.environment["CDSP_FILTER_BIN"] { return env }
    return harnessPath(named: "cdsp_filter_compare")
  }

  /// Locate a Rust harness binary alongside this test file. See the comment
  /// in `ResamplerComparisonTests.harnessBinary(named:)` for the layout.
  static func harnessPath(named name: String, file: String = #filePath) -> String {
    let url = URL(fileURLWithPath: file)
      .deletingLastPathComponent()  // DSPMonitorTests
      .deletingLastPathComponent()  // Tests
      .appendingPathComponent("RustHarnesses/target/release/\(name)")
    return url.path
  }

  // MARK: - Helpers

  private func writeRaw(_ data: [Double], to path: String) throws {
    let buf = data.withUnsafeBufferPointer { Data(buffer: $0) }
    try buf.write(to: URL(fileURLWithPath: path))
  }

  private func readRaw(from path: String) throws -> [Double] {
    let data = try Data(contentsOf: URL(fileURLWithPath: path))
    let count = data.count / MemoryLayout<Double>.stride
    return data.withUnsafeBytes { raw -> [Double] in
      let p = raw.bindMemory(to: Double.self)
      return Array(UnsafeBufferPointer(start: p.baseAddress, count: count))
    }
  }

  private func runHarness(_ args: [String]) throws -> Bool {
    let bin = Self.harnessBinary
    guard FileManager.default.isExecutableFile(atPath: bin) else {
      print(
        "⚠️ skipping: harness not found at \(bin) — build with `cd ~/cdsp_filter_compare && cargo build --release`"
      )
      return false
    }
    let proc = Process()
    proc.executableURL = URL(fileURLWithPath: bin)
    proc.arguments = args
    let stderr = Pipe()
    proc.standardError = stderr
    try proc.run()
    proc.waitUntilExit()
    if proc.terminationStatus != 0 {
      let err =
        String(data: stderr.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
      Issue.record("harness exited with \(proc.terminationStatus): \(err)")
      return false
    }
    return true
  }

  /// Generate a deterministic, mildly noisy test signal: sum of three sines plus
  /// low-amplitude white noise. Exercises a filter at multiple frequencies in a
  /// single pass.
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

  // MARK: - Biquad

  /// Apply a Biquad filter with raw a/b coefficients, comparing Swift output
  /// against camilladsp's `Biquad`. The two implementations should match to
  /// FP precision since they both compute Direct Form I from the same coeffs.
  @Test func Biquad_RawCoeffs_Lowpass1kHz() throws {
    // Lowpass coefficients computed offline for 1 kHz, Q=0.707, fs=48 kHz
    // using the Robert Bristow-Johnson cookbook formulas. Both implementations
    // should produce the same output given these.
    let b0 = 0.004244741301241303
    let b1 = 0.008489482602482605
    let b2 = 0.004244741301241303
    let a1 = -1.864844640491105
    let a2 = 0.8818236057002321
    try compareBiquad(b0: b0, b1: b1, b2: b2, a1: a1, a2: a2, label: "lowpass-1k")
  }

  @Test func Biquad_RawCoeffs_Highpass5kHz() throws {
    // Highpass at 5 kHz, Q=0.707, fs=48 kHz.
    let b0 = 0.7392382866526886
    let b1 = -1.4784765733053772
    let b2 = 0.7392382866526886
    let a1 = -1.4042598022895725
    let a2 = 0.5526933443211819
    try compareBiquad(b0: b0, b1: b1, b2: b2, a1: a1, a2: a2, label: "highpass-5k")
  }

  @Test func Biquad_RawCoeffs_Peaking2kHz() throws {
    // Peaking +6 dB at 2 kHz, Q=2, fs=48 kHz.
    let b0 = 1.0480378925069767
    let b1 = -1.9266017680029408
    let b2 = 0.9043155506796712
    let a1 = -1.9266017680029408
    let a2 = 0.9523534431866478
    try compareBiquad(b0: b0, b1: b1, b2: b2, a1: a1, a2: a2, label: "peaking-2k")
  }

  private func compareBiquad(
    b0: Double, b1: Double, b2: Double, a1: Double, a2: Double, label: String
  ) throws {
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_biquad_\(label)_in.raw"
    let refPath = "/tmp/cdsp_biquad_\(label)_ref.raw"
    try writeRaw(input, to: inPath)

    guard
      try runHarness([
        "biquad",
        String(a1), String(a2), String(b0), String(b1), String(b2),
        String(Self.sampleRate), String(Self.chunkSize),
        inPath, refPath,
      ])
    else {
      if true { return }
      _ = ("harness binary missing")
    }
    let ref = try readRaw(from: refPath)

    // Run the same filter in Swift. Use the scalar DF2T path so the output
    // matches.
    let coeffs = BiquadCoefficients(b0: b0, b1: b1, b2: b2, a1: a1, a2: a2)
    let filter = BiquadFilter(coefficients: coeffs)
    var swiftOut = input
    var idx = 0
    while idx < swiftOut.count {
      let end = min(idx + Self.chunkSize, swiftOut.count)
      var slice = Array(swiftOut[idx..<end])
      filter.process(waveform: &slice)
      for (i, v) in slice.enumerated() { swiftOut[idx + i] = v }
      idx = end
    }

    #expect(swiftOut.count == ref.count)
    var maxAbsDiff = 0.0
    var sumSq = 0.0
    for i in 0..<min(swiftOut.count, ref.count) {
      let d = swiftOut[i] - ref[i]
      maxAbsDiff = max(maxAbsDiff, abs(d))
      sumSq += d * d
    }
    let rms = sqrt(sumSq / Double(swiftOut.count))
    print(
      String(
        format: "[biquad %@] maxAbsDiff=%.3e rms=%.3e (n=%d)",
        label, maxAbsDiff, rms, swiftOut.count))
    // Direct Form I and DF2T are mathematically equivalent but not bit-exact
    // due to floating-point rounding differences.
    #expect(maxAbsDiff < 1e-13)
  }

  // MARK: - Gain

  @Test func Gain_Plus6dB() throws {
    try compareGain(gainDB: 6.0, inverted: false, mute: false, label: "+6dB")
  }

  @Test func Gain_Minus12dB_Inverted() throws {
    try compareGain(gainDB: -12.0, inverted: true, mute: false, label: "-12dB-inv")
  }

  @Test func Gain_Mute() throws {
    try compareGain(gainDB: 3.0, inverted: false, mute: true, label: "mute")
  }

  private func compareGain(gainDB: Double, inverted: Bool, mute: Bool, label: String) throws {
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_gain_\(label)_in.raw"
    let refPath = "/tmp/cdsp_gain_\(label)_ref.raw"
    try writeRaw(input, to: inPath)

    guard
      try runHarness([
        "gain",
        String(gainDB), inverted ? "1" : "0", mute ? "1" : "0",
        String(Self.chunkSize), inPath, refPath,
      ])
    else {
      if true { return }
      _ = ("harness binary missing")
    }
    let ref = try readRaw(from: refPath)

    var fp = GainParameters()
    fp.gain = gainDB
    fp.inverted = inverted
    fp.mute = mute
    fp.scale = .dB
    let filter = GainFilter(parameters: fp)
    var swiftOut = input
    var idx = 0
    while idx < swiftOut.count {
      let end = min(idx + Self.chunkSize, swiftOut.count)
      var slice = Array(swiftOut[idx..<end])
      filter.process(waveform: &slice)
      for (i, v) in slice.enumerated() { swiftOut[idx + i] = v }
      idx = end
    }

    var maxAbsDiff = 0.0
    for i in 0..<min(swiftOut.count, ref.count) {
      maxAbsDiff = max(maxAbsDiff, abs(swiftOut[i] - ref[i]))
    }
    print(String(format: "[gain %@] maxAbsDiff=%.3e (n=%d)", label, maxAbsDiff, swiftOut.count))
    // Scalar multiply on identical inputs should agree exactly.
    #expect(maxAbsDiff < 1e-12)
  }

  // MARK: - Volume
  //
  // The Rust harness forces ramp_time_ms=0, so Volume always takes the
  // constant-gain branch. Swift's VolumeFilter constructed via the
  // direct-init initialiser with no ramping behaves the same way: target
  // volume == current volume from the start, single multiply per chunk.

  @Test func Volume_Plus3dB() throws {
    try compareVolume(currentVolumeDB: 3.0, mute: false, label: "+3dB")
  }

  @Test func Volume_Minus20dB() throws {
    try compareVolume(currentVolumeDB: -20.0, mute: false, label: "-20dB")
  }

  @Test func Volume_Mute() throws {
    try compareVolume(currentVolumeDB: 0.0, mute: true, label: "mute")
  }

  private func compareVolume(currentVolumeDB: Double, mute: Bool, label: String) throws {
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_volume_\(label)_in.raw"
    let refPath = "/tmp/cdsp_volume_\(label)_ref.raw"
    try writeRaw(input, to: inPath)
    guard
      try runHarness([
        "volume",
        String(currentVolumeDB), mute ? "1" : "0",
        String(Self.sampleRate), String(Self.chunkSize),
        inPath, refPath,
      ])
    else {
      if true { return }
      _ = ("harness binary missing")
    }
    let ref = try readRaw(from: refPath)

    // Build the Swift VolumeFilter via the direct initialiser with
    // rampTimeMs=0 — matches the Rust harness's ramp_time_ms=0 path so the
    // filter applies its target_linear_gain immediately.
    let params = ProcessingParameters(captureChannels: 1, playbackChannels: 1)
    params.targetVolume = currentVolumeDB
    params.isMuted = mute
    params.currentVolume = mute ? -100.0 : currentVolumeDB
    let filter = VolumeFilter(processingParameters: params)

    var swiftOut = input
    var idx = 0
    while idx < swiftOut.count {
      let end = min(idx + Self.chunkSize, swiftOut.count)
      var slice = Array(swiftOut[idx..<end])
      filter.process(waveform: &slice)
      for (i, v) in slice.enumerated() { swiftOut[idx + i] = v }
      idx = end
    }

    var maxAbsDiff = 0.0
    for i in 0..<min(swiftOut.count, ref.count) {
      maxAbsDiff = max(maxAbsDiff, abs(swiftOut[i] - ref[i]))
    }
    print(
      String(format: "[volume %@] maxAbsDiff=%.3e (n=%d)", label, maxAbsDiff, swiftOut.count))
    // Constant gain — bit-exact match expected.
    #expect(maxAbsDiff < 1e-12)
  }

  // MARK: - Loudness
  //
  // Loudness is a cascade: highshelf @ 3500 Hz + lowshelf @ 70 Hz, both at
  // 12 dB/oct slope, each scaled by `relBoost(currentVolume, refLevel) * boost`.
  // With the harness pinning current_volume on construction (and our Swift
  // setup mirroring it), both implementations should compute the same biquad
  // coefficients and then run identical Direct Form I biquads.

  @Test func Loudness_BelowReference_Active() throws {
    // Volume 10 dB below reference → relboost = 0.5, both shelves boosted.
    try compareLoudness(
      volumeDB: -35.0, referenceDB: -25.0, highBoost: 10.0, lowBoost: 10.0,
      attenuateMid: false, label: "below-ref")
  }

  @Test func Loudness_AtReference_NoBoost() throws {
    // Volume == reference → relboost = 0 → filter inactive (passthrough).
    try compareLoudness(
      volumeDB: -25.0, referenceDB: -25.0, highBoost: 10.0, lowBoost: 10.0,
      attenuateMid: false, label: "at-ref")
  }

  @Test func Loudness_AttenuateMid() throws {
    try compareLoudness(
      volumeDB: -45.0, referenceDB: -25.0, highBoost: 10.0, lowBoost: 10.0,
      attenuateMid: true, label: "attenuate-mid")
  }

  private func compareLoudness(
    volumeDB: Double, referenceDB: Double,
    highBoost: Double, lowBoost: Double, attenuateMid: Bool, label: String
  ) throws {
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_loudness_\(label)_in.raw"
    let refPath = "/tmp/cdsp_loudness_\(label)_ref.raw"
    try writeRaw(input, to: inPath)
    guard
      try runHarness([
        "loudness",
        String(volumeDB), String(referenceDB),
        String(highBoost), String(lowBoost),
        attenuateMid ? "1" : "0",
        String(Self.sampleRate), String(Self.chunkSize),
        inPath, refPath,
      ])
    else {
      if true { return }
      _ = ("harness binary missing")
    }
    let ref = try readRaw(from: refPath)

    // Build a Swift LoudnessFilter at the matching volume.
    var lp = LoudnessParameters()
    lp.referenceLevel = referenceDB
    lp.highBoost = highBoost
    lp.lowBoost = lowBoost
    lp.attenuateMid = attenuateMid
    let params = ProcessingParameters(captureChannels: 1, playbackChannels: 1)
    params.currentVolume = volumeDB

    let filter = LoudnessFilter(
      parameters: lp,
      sampleRate: Self.sampleRate)
    filter.processingParameters = params
    // Trigger `updateShelves()` so the internal biquads' coefficients are
    // populated.
    var primer = [PrcFmt](repeating: 0, count: 8)
    filter.process(waveform: &primer)

    var swiftOut = input
    var idx = 0
    while idx < swiftOut.count {
      let end = min(idx + Self.chunkSize, swiftOut.count)
      var slice = Array(swiftOut[idx..<end])
      filter.process(waveform: &slice)
      for (i, v) in slice.enumerated() { swiftOut[idx + i] = v }
      idx = end
    }

    var maxAbsDiff = 0.0
    var sumSq = 0.0
    for i in 0..<min(swiftOut.count, ref.count) {
      let d = swiftOut[i] - ref[i]
      maxAbsDiff = max(maxAbsDiff, abs(d))
      sumSq += d * d
    }
    let rms = sqrt(sumSq / Double(swiftOut.count))
    print(
      String(
        format: "[loudness %@] maxAbsDiff=%.3e rms=%.3e (n=%d)",
        label, maxAbsDiff, rms, swiftOut.count))
    // Two cascaded biquads with identical coefficients; expect <~1e-9 abs diff.
    #expect(maxAbsDiff < 1e-9)
  }

  // MARK: - Mixer
  //
  // The mixer is rate- and channel-routing only with a linear sum per output
  // channel: out[d][n] = sum_{s in mapping[d]} input[s.channel][n] * s.gain
  // where `s.gain = pow(10, gain_db/20)` for dB scale, optionally negated when
  // inverted, zero when muted. We compute the analytical reference inline (no
  // Rust harness needed) and assert the Swift mixer matches exactly.

  @Test func Mixer_Vs_AnalyticalReference_StereoToMono() throws {
    // Stereo (L,R) → mono with L at 0 dB, R at -6 dB.
    let config = MixerConfig(
      channelsIn: 2, channelsOut: 1,
      mapping: [
        MixerMapping(
          dest: 0,
          sources: [
            MixerSource(channel: 0, gain: 0.0),
            MixerSource(channel: 1, gain: -6.0),
          ])
      ])
    let mixer = AudioMixer(config: config, chunkSize: 2048)

    var rng = SeededRNG(seed: 0xC0FFEE)
    let l = (0..<1024).map { _ in Double.random(in: -1.0...1.0, using: &rng) }
    let r = (0..<1024).map { _ in Double.random(in: -1.0...1.0, using: &rng) }
    let chunk = AudioChunk(waveforms: [l, r], validFrames: 1024)
    let out = mixer.process(chunk: chunk)

    let gainR = pow(10.0, -6.0 / 20.0)
    for i in 0..<1024 {
      let expected = l[i] + gainR * r[i]
      #expect(abs(out.waveforms[0][i] - expected) <= 1e-12)
    }
  }

  @Test func Mixer_Vs_AnalyticalReference_LinearScale() throws {
    // Linear scale (not dB). Output 0 = 0.5 * in[0] - 0.25 * in[1] (inverted).
    let config = MixerConfig(
      channelsIn: 2, channelsOut: 1,
      mapping: [
        MixerMapping(
          dest: 0,
          sources: [
            MixerSource(channel: 0, gain: 0.5, scale: .linear),
            MixerSource(channel: 1, gain: 0.25, inverted: true, scale: .linear),
          ])
      ])
    let mixer = AudioMixer(config: config, chunkSize: 2048)

    let l = (0..<128).map { Double($0) * 0.01 }
    let r = (0..<128).map { Double($0) * -0.005 }
    let chunk = AudioChunk(waveforms: [l, r], validFrames: 128)
    let out = mixer.process(chunk: chunk)

    for i in 0..<128 {
      let expected = 0.5 * l[i] - 0.25 * r[i]
      #expect(abs(out.waveforms[0][i] - expected) <= 1e-12)
    }
  }

  @Test func Mixer_MutedSource_ProducesSilenceFromThatSource() throws {
    // Two sources into one output, the second is muted. Output = source 0 only.
    let config = MixerConfig(
      channelsIn: 2, channelsOut: 1,
      mapping: [
        MixerMapping(
          dest: 0,
          sources: [
            MixerSource(channel: 0, gain: 0.0),
            MixerSource(channel: 1, gain: 0.0, mute: true),
          ])
      ])
    let mixer = AudioMixer(config: config, chunkSize: 2048)

    let l = (0..<64).map { Double($0) }
    let r = (0..<64).map { _ in 999.0 }  // would dominate if not muted
    let chunk = AudioChunk(waveforms: [l, r], validFrames: 64)
    let out = mixer.process(chunk: chunk)

    for i in 0..<64 {
      #expect(abs(out.waveforms[0][i] - l[i]) <= 1e-12)
    }
  }

}

/// Deterministic RNG so test signals are bit-reproducible across runs.
private struct SeededRNG: RandomNumberGenerator {
  var state: UInt64
  init(seed: UInt64) { self.state = seed }
  mutating func next() -> UInt64 {
    state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
    return state
  }
}
