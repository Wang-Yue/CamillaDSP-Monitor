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
@testable import DSPProcessors

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
    let volParams = VolumeParameters(rampTime: 0.0, limit: 50.0, fader: .main)
    let filter = VolumeFilter(
      parameters: volParams,
      sampleRate: Self.sampleRate,
      chunkSize: Self.chunkSize,
      processingParameters: params
    )

    var swiftOut = input
    var idx = 0
    while idx < swiftOut.count {
      let end = min(idx + Self.chunkSize, swiftOut.count)
      var slice = Array(swiftOut[idx..<end])
      filter.prepareChunk()
      filter.process(waveform: &slice)
      filter.advanceRamp()
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

  // MARK: - Convolution

  @Test func Convolution_Vs_Rust_RandomIR() throws {
    let label = "conv-random"
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_conv_\(label)_in.raw"
    let refPath = "/tmp/cdsp_conv_\(label)_ref.raw"
    let coeffsPath = "/tmp/cdsp_conv_\(label)_coeffs.raw"
    try writeRaw(input, to: inPath)

    // Generate random IR coefficients of length 2000.
    var rng = SeededRNG(seed: 0x1234_5678_9ABC_DEF0)
    let coeffs = (0..<2000).map { _ in Double.random(in: -1.0...1.0, using: &rng) }
    try writeRaw(coeffs, to: coeffsPath)

    guard
      try runHarness([
        "conv",
        String(Self.chunkSize), coeffsPath, inPath, refPath,
      ])
    else {
      if true { return }
      _ = ("harness binary missing")
    }
    let ref = try readRaw(from: refPath)

    let filter = ConvolutionFilter(coefficients: coeffs, chunkSize: Self.chunkSize)
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
        format: "[conv %@] maxAbsDiff=%.3e rms=%.3e (n=%d)",
        label, maxAbsDiff, rms, swiftOut.count))
    // Un-normalised Stockham-style segmented overlap-save convolution using double precision
    // real FFTs. Slight rounding differences from SIMD/FFT twiddle layout are expected, but should
    // match closely.
    #expect(maxAbsDiff < 1e-13)
  }

  // MARK: - Delay

  @Test func Delay_IntegerSamples() throws {
    try compareDelay(delay: 50.0, unit: .samples, subsample: false, label: "50samples")
  }

  @Test func Delay_Subsample_FirstOrder() throws {
    // 0.6 samples delay -> exercises 1st order Thiran allpass
    try compareDelay(delay: 0.6, unit: .samples, subsample: true, label: "0.6samples")
  }

  @Test func Delay_Subsample_SecondOrder() throws {
    // 2.3 samples delay -> exercises 2nd order Thiran allpass
    try compareDelay(delay: 2.3, unit: .samples, subsample: true, label: "2.3samples")
  }

  @Test func Delay_Milliseconds() throws {
    try compareDelay(delay: 1.5, unit: .ms, subsample: true, label: "1.5ms")
  }

  private func compareDelay(delay: Double, unit: DelayUnit, subsample: Bool, label: String) throws {
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_delay_\(label)_in.raw"
    let refPath = "/tmp/cdsp_delay_\(label)_ref.raw"
    try writeRaw(input, to: inPath)

    guard
      try runHarness([
        "delay",
        String(delay), unit.rawValue, subsample ? "1" : "0",
        String(Self.sampleRate), String(Self.chunkSize),
        inPath, refPath,
      ])
    else {
      return
    }
    let ref = try readRaw(from: refPath)

    let params = DelayParameters(delay: delay, unit: unit, subsample: subsample)
    let filter = DelayFilter(parameters: params, sampleRate: Self.sampleRate)

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
    for i in 0..<min(swiftOut.count, ref.count) {
      maxAbsDiff = max(maxAbsDiff, abs(swiftOut[i] - ref[i]))
    }
    print(String(format: "[delay %@] maxAbsDiff=%.3e", label, maxAbsDiff))
    #expect(maxAbsDiff < 1e-12)
  }

  // MARK: - Biquad Combo

  @Test func BiquadCombo_ButterworthLowpass() throws {
    try compareBiquadCombo(type: .butterworthLowpass, freq: 1200.0, order: 4, label: "bw-lp")
  }

  @Test func BiquadCombo_ButterworthHighpass() throws {
    try compareBiquadCombo(type: .butterworthHighpass, freq: 600.0, order: 3, label: "bw-hp")
  }

  @Test func BiquadCombo_LinkwitzRileyLowpass() throws {
    try compareBiquadCombo(type: .linkwitzRileyLowpass, freq: 2000.0, order: 4, label: "lr-lp")
  }

  @Test func BiquadCombo_LinkwitzRileyHighpass() throws {
    try compareBiquadCombo(type: .linkwitzRileyHighpass, freq: 1500.0, order: 2, label: "lr-hp")
  }

  @Test func BiquadCombo_Tilt() throws {
    try compareBiquadCombo(type: .tilt, gain: 4.5, label: "tilt")
  }

  @Test func BiquadCombo_GraphicEqualizer() throws {
    let gains = [1.0, -2.0, 3.0, -1.5, 0.5]
    try compareBiquadCombo(
      type: .graphicEqualizer, freqMin: 20.0, freqMax: 20000.0, gains: gains, label: "geq",
      epsilon: 1e-7)
  }

  @Test func BiquadCombo_FivePointPeq() throws {
    try compareBiquadCombo(
      type: .fivePointPeq,
      fls: 80.0, qls: 0.707, gls: 3.0,
      fp1: 250.0, qp1: 1.5, gp1: -2.0,
      fp2: 1000.0, qp2: 2.0, gp2: 1.5,
      fp3: 4000.0, qp3: 1.0, gp3: -1.0,
      fhs: 12000.0, qhs: 0.707, ghs: 2.5,
      label: "peq5"
    )
  }

  private func compareBiquadCombo(
    type: BiquadComboType,
    freq: Double? = nil,
    order: Int? = nil,
    gain: Double? = nil,
    fls: Double? = nil, qls: Double? = nil, gls: Double? = nil,
    fp1: Double? = nil, qp1: Double? = nil, gp1: Double? = nil,
    fp2: Double? = nil, qp2: Double? = nil, gp2: Double? = nil,
    fp3: Double? = nil, qp3: Double? = nil, gp3: Double? = nil,
    fhs: Double? = nil, qhs: Double? = nil, ghs: Double? = nil,
    freqMin: Double? = nil, freqMax: Double? = nil, gains: [Double]? = nil,
    label: String,
    epsilon: Double = 1e-12
  ) throws {
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_combo_\(label)_in.raw"
    let refPath = "/tmp/cdsp_combo_\(label)_ref.raw"
    try writeRaw(input, to: inPath)

    var harnessArgs: [String] = ["biquad_combo"]
    switch type {
    case .butterworthLowpass:
      harnessArgs.append(contentsOf: ["butterworth_lowpass", String(freq!), String(order!)])
    case .butterworthHighpass:
      harnessArgs.append(contentsOf: ["butterworth_highpass", String(freq!), String(order!)])
    case .linkwitzRileyLowpass:
      harnessArgs.append(contentsOf: ["linkwitz_riley_lowpass", String(freq!), String(order!)])
    case .linkwitzRileyHighpass:
      harnessArgs.append(contentsOf: ["linkwitz_riley_highpass", String(freq!), String(order!)])
    case .tilt:
      harnessArgs.append(contentsOf: ["tilt", String(gain!)])
    case .fivePointPeq:
      harnessArgs.append(contentsOf: [
        "five_point_peq",
        String(fls!), String(qls!), String(gls!),
        String(fp1!), String(qp1!), String(gp1!),
        String(fp2!), String(qp2!), String(gp2!),
        String(fp3!), String(qp3!), String(gp3!),
        String(fhs!), String(qhs!), String(ghs!),
      ])
    case .graphicEqualizer:
      let gainsStr = gains!.map { String($0) }.reduce("") { $0.isEmpty ? $1 : $0 + "," + $1 }
      harnessArgs.append(contentsOf: [
        "graphic_equalizer", String(freqMin!), String(freqMax!), gainsStr,
      ])
    }
    harnessArgs.append(contentsOf: [
      String(Self.sampleRate), String(Self.chunkSize), inPath, refPath,
    ])

    guard try runHarness(harnessArgs) else { return }
    let ref = try readRaw(from: refPath)

    let params = BiquadComboParameters(
      type: type, freq: freq, order: order, gain: gain,
      fls: fls, qls: qls, gls: gls,
      fp1: fp1, qp1: qp1, gp1: gp1,
      fp2: fp2, qp2: qp2, gp2: gp2,
      fp3: fp3, qp3: qp3, gp3: gp3,
      fhs: fhs, qhs: qhs, ghs: ghs,
      freqMin: freqMin, freqMax: freqMax, gains: gains
    )
    let filter = try BiquadComboFilter(parameters: params, sampleRate: Self.sampleRate)

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
    for i in 0..<min(swiftOut.count, ref.count) {
      maxAbsDiff = max(maxAbsDiff, abs(swiftOut[i] - ref[i]))
    }
    print(String(format: "[biquad_combo %@] maxAbsDiff=%.3e", label, maxAbsDiff))
    #expect(maxAbsDiff < epsilon)
  }

  // MARK: - DiffEq

  @Test func DiffEq_SimpleIIR() throws {
    let a = [1.0, -1.864844640491105, 0.8818236057002321]
    let b = [0.004244741301241303, 0.008489482602482605, 0.004244741301241303]
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_diffeq_in.raw"
    let refPath = "/tmp/cdsp_diffeq_ref.raw"
    try writeRaw(input, to: inPath)

    let aStr = a.map { String($0) }.reduce("") { $0.isEmpty ? $1 : $0 + "," + $1 }
    let bStr = b.map { String($0) }.reduce("") { $0.isEmpty ? $1 : $0 + "," + $1 }
    guard
      try runHarness([
        "diff_eq",
        aStr, bStr,
        String(Self.chunkSize), inPath, refPath,
      ])
    else {
      return
    }
    let ref = try readRaw(from: refPath)

    let params = DiffEqParameters(a: a, b: b)
    let filter = DiffEqFilter(parameters: params)

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
    for i in 0..<min(swiftOut.count, ref.count) {
      maxAbsDiff = max(maxAbsDiff, abs(swiftOut[i] - ref[i]))
    }
    print(String(format: "[diffeq] maxAbsDiff=%.3e", maxAbsDiff))
    #expect(maxAbsDiff < 1e-12)
  }

  // MARK: - Dither

  @Test func Dither_None() throws {
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_dither_none_in.raw"
    let refPath = "/tmp/cdsp_dither_none_ref.raw"
    try writeRaw(input, to: inPath)

    guard
      try runHarness([
        "dither",
        "none", "16",
        String(Self.chunkSize), inPath, refPath,
      ])
    else {
      return
    }
    let ref = try readRaw(from: refPath)

    let params = DitherParameters(type: .none, bits: 16)
    let filter = DitherFilter(parameters: params)

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
    for i in 0..<min(swiftOut.count, ref.count) {
      maxAbsDiff = max(maxAbsDiff, abs(swiftOut[i] - ref[i]))
    }
    print(String(format: "[dither none] maxAbsDiff=%.3e", maxAbsDiff))
    #expect(maxAbsDiff < 1e-12)
  }

  // MARK: - Limiter

  @Test func Limiter_HardClip() throws {
    try compareLimiter(clipLimit: -3.0, softClip: false, label: "hard")
  }

  @Test func Limiter_SoftClip() throws {
    try compareLimiter(clipLimit: -1.5, softClip: true, label: "soft")
  }

  private func compareLimiter(clipLimit: Double, softClip: Bool, label: String) throws {
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_limiter_\(label)_in.raw"
    let refPath = "/tmp/cdsp_limiter_\(label)_ref.raw"
    try writeRaw(input, to: inPath)

    guard
      try runHarness([
        "limiter",
        String(clipLimit), softClip ? "1" : "0",
        String(Self.chunkSize), inPath, refPath,
      ])
    else {
      return
    }
    let ref = try readRaw(from: refPath)

    let params = LimiterParameters(clipLimit: clipLimit, softClip: softClip)
    let filter = LimiterFilter(parameters: params)

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
    for i in 0..<min(swiftOut.count, ref.count) {
      maxAbsDiff = max(maxAbsDiff, abs(swiftOut[i] - ref[i]))
    }
    print(String(format: "[limiter %@] maxAbsDiff=%.3e", label, maxAbsDiff))
    #expect(maxAbsDiff < 1e-12)
  }

  // MARK: - Lookahead Limiter

  @Test func LookaheadLimiter_Basic() throws {
    try compareLookaheadLimiter(
      limit: -1.0, attack: 4.0, release: 20.0, unit: .samples, label: "basic")
  }

  @Test func LookaheadLimiter_Instant() throws {
    try compareLookaheadLimiter(
      limit: -2.0, attack: 0.0, release: 0.0, unit: .samples, label: "instant")
  }

  private func compareLookaheadLimiter(
    limit: Double, attack: Double, release: Double, unit: DelayUnit, label: String
  ) throws {
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_lookahead_\(label)_in.raw"
    let refPath = "/tmp/cdsp_lookahead_\(label)_ref.raw"
    try writeRaw(input, to: inPath)

    guard
      try runHarness([
        "lookahead_limiter",
        String(limit), String(attack), String(release), unit.rawValue,
        String(Self.sampleRate), String(Self.chunkSize),
        inPath, refPath,
      ])
    else {
      return
    }
    let ref = try readRaw(from: refPath)

    let params = LookaheadLimiterParameters(
      limit: limit, attack: attack, release: release, unit: unit)
    let filter = LookaheadLimiterFilter(
      parameters: params, sampleRate: Self.sampleRate, chunkSize: Self.chunkSize)

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
    for i in 0..<min(swiftOut.count, ref.count) {
      maxAbsDiff = max(maxAbsDiff, abs(swiftOut[i] - ref[i]))
    }
    print(String(format: "[lookahead %@] maxAbsDiff=%.3e", label, maxAbsDiff))
    #expect(maxAbsDiff < 1e-5)
  }

  // MARK: - Processors

  @Test func Compressor_Vs_RustReference() throws {
    let label = "compressor-compare"
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_comp_\(label)_in.raw"
    let refPath = "/tmp/cdsp_comp_\(label)_ref.raw"
    try writeRaw(input, to: inPath)

    let attack = 0.005
    let release = 0.05
    let threshold = -10.0
    let factor = 3.0
    let makeupGain = 2.0
    let softClip = true
    let clipLimit = -1.0

    guard
      try runHarness([
        "compressor",
        String(attack), String(release), String(threshold), String(factor), String(makeupGain),
        "1", String(clipLimit),
        String(Self.sampleRate), String(Self.chunkSize),
        inPath, refPath,
      ])
    else {
      return
    }
    let ref = try readRaw(from: refPath)

    let params = CompressorParameters(
      channels: 1,
      monitorChannels: nil,
      processChannels: nil,
      attack: attack,
      release: release,
      threshold: threshold,
      factor: factor,
      makeupGain: makeupGain,
      softClip: softClip,
      clipLimit: clipLimit
    )
    let compressor = CompressorProcessor(
      parameters: params, sampleRate: Self.sampleRate, chunkSize: Self.chunkSize)

    var chunk = AudioChunk(frames: input.count, channels: 1)
    for i in 0..<input.count {
      chunk[0][i] = input[i]
    }

    try! compressor.process(chunk: &chunk)

    #expect(chunk.validFrames == ref.count)
    var maxAbsDiff = 0.0
    for i in 0..<min(ref.count, chunk.validFrames) {
      maxAbsDiff = max(maxAbsDiff, abs(chunk[0][i] - ref[i]))
    }
    print(String(format: "[compressor] maxAbsDiff=%.3e", maxAbsDiff))
    #expect(maxAbsDiff < 1e-12)
  }

  @Test func NoiseGate_Vs_RustReference() throws {
    let label = "noisegate-compare"
    let input = makeTestSignal()
    let inPath = "/tmp/cdsp_gate_\(label)_in.raw"
    let refPath = "/tmp/cdsp_gate_\(label)_ref.raw"
    try writeRaw(input, to: inPath)

    let attack = 0.005
    let release = 0.05
    let threshold = -24.0
    let attenuation = 20.0

    guard
      try runHarness([
        "noisegate",
        String(attack), String(release), String(threshold), String(attenuation),
        String(Self.sampleRate), String(Self.chunkSize),
        inPath, refPath,
      ])
    else {
      return
    }
    let ref = try readRaw(from: refPath)

    let params = NoiseGateParameters(
      channels: 1,
      monitorChannels: nil,
      processChannels: nil,
      attack: attack,
      release: release,
      threshold: threshold,
      attenuation: attenuation
    )
    let gate = NoiseGateProcessor(
      parameters: params, sampleRate: Self.sampleRate, chunkSize: Self.chunkSize)

    var chunk = AudioChunk(frames: input.count, channels: 1)
    for i in 0..<input.count {
      chunk[0][i] = input[i]
    }

    try! gate.process(chunk: &chunk)

    #expect(chunk.validFrames == ref.count)
    var maxAbsDiff = 0.0
    for i in 0..<min(ref.count, chunk.validFrames) {
      maxAbsDiff = max(maxAbsDiff, abs(chunk[0][i] - ref[i]))
    }
    print(String(format: "[noisegate] maxAbsDiff=%.3e", maxAbsDiff))
    #expect(maxAbsDiff < 1e-12)
  }

  @Test func RACE_Vs_RustReference() throws {
    let label = "race-compare"
    let input = makeTestSignal()

    let input0 = input
    let input1 = input.map { $0 * 0.5 }

    let inPath0 = "/tmp/cdsp_race_\(label)_in0.raw"
    let inPath1 = "/tmp/cdsp_race_\(label)_in1.raw"
    let refPath0 = "/tmp/cdsp_race_\(label)_ref0.raw"
    let refPath1 = "/tmp/cdsp_race_\(label)_ref1.raw"

    try writeRaw(input0, to: inPath0)
    try writeRaw(input1, to: inPath1)

    let delay = 12.0
    let attenuation = 8.5

    guard
      try runHarness([
        "race",
        "0", "1", String(delay), "samples", "0", String(attenuation),
        String(Self.sampleRate), String(Self.chunkSize),
        inPath0, inPath1, refPath0, refPath1,
      ])
    else {
      return
    }
    let ref0 = try readRaw(from: refPath0)
    let ref1 = try readRaw(from: refPath1)

    let params = RACEParameters(
      channels: 2,
      channelA: 0,
      channelB: 1,
      delay: delay,
      subsampleDelay: false,
      delayUnit: .samples,
      attenuation: attenuation
    )
    let race = try! RACEProcessor(parameters: params, sampleRate: Self.sampleRate)

    var chunk = AudioChunk(frames: input0.count, channels: 2)
    for i in 0..<input0.count {
      chunk[0][i] = input0[i]
      chunk[1][i] = input1[i]
    }

    try! race.process(chunk: &chunk)

    #expect(chunk.validFrames == ref0.count)
    var maxAbsDiff0 = 0.0
    var maxAbsDiff1 = 0.0
    for i in 0..<min(ref0.count, chunk.validFrames) {
      maxAbsDiff0 = max(maxAbsDiff0, abs(chunk[0][i] - ref0[i]))
      maxAbsDiff1 = max(maxAbsDiff1, abs(chunk[1][i] - ref1[i]))
    }
    print(String(format: "[race] maxAbsDiff ch0=%.3e ch1=%.3e", maxAbsDiff0, maxAbsDiff1))
    #expect(maxAbsDiff0 < 1e-12)
    #expect(maxAbsDiff1 < 1e-12)
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
