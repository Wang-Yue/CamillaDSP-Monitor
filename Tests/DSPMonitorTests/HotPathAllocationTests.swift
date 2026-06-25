// Hot-path allocation hygiene tests. Each test exercises a component's
// per-chunk inner loop under `AllocationCounter` and asserts the body is
// allocation-free in steady state.
//
// The `< 10` bound is enforced only in release builds. In debug, Swift
// doesn't inline `@inlinable` Array helpers (notably
// `Array.withUnsafeBufferPointer`), so closure-context heap boxes plus
// extra ARC traffic on captured Array storage refs produce thousands of
// allocations per call that disappear entirely under `-c release`. The
// debug path still prints the count so a curious reader sees it; only
// release fails on regression.

import Darwin
import Foundation
import Synchronization
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters
@testable import DSPLogging
@testable import DSPMixer
@testable import DSPResampler

// MARK: - Allocation counter
//
// Per-thread heap allocation counter, used by the tests below to assert
// hot-path code is allocation-free.
//
// Hooks `malloc_logger`, the same global function pointer Instruments'
// Allocations track uses to observe every malloc / free / realloc in the
// process. The hook is documented in Apple's open-source libmalloc and
// has been stable for many macOS releases, but the symbol is technically
// private — we look it up via `dlsym` so the helper fails closed
// (returning `nil` from `count`) on a future OS that no longer exposes
// it, rather than crashing the test suite.
//
// The callback fires on whichever thread invoked the underlying malloc,
// so we filter by `pthread_self()` to count only allocations performed
// on the thread that called `count(...)`. This keeps the measurement
// meaningful when other threads (background Dispatch queues, Swift
// runtime workers) happen to allocate during the same window.
//
// Why allocation count and not wall-clock variance: variance-based
// proxies are flaky under scheduler load. Counting alloc events is a
// direct measurement and it catches transient `Array` CoW patterns
// (alloc-then-free inside the same call) that a `blocks_in_use`
// snapshot would miss.

private enum AllocationCounter {
  /// Matches the libmalloc signature: `void (*)(uint32_t type,
  /// uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t result,
  /// uint32_t num_hot_frames_to_skip)`.
  fileprivate typealias MallocLogger =
    @convention(c) (
      UInt32, UInt, UInt, UInt, UInt, UInt32
    ) -> Void

  /// `MALLOC_LOG_TYPE_ALLOCATE` from libmalloc's private header. Set on
  /// `malloc` / `calloc` events and on the alloc half of `realloc`
  /// (which is reported as `ALLOCATE | DEALLOCATE = 6`).
  private static let allocBit: UInt32 = 2

  /// `&malloc_logger`, found via `dlsym`. `nil` means the hook isn't
  /// exposed by the loaded libmalloc — `count(_:)` returns `nil` so
  /// callers can skip the assertion gracefully.
  ///
  /// `nonisolated(unsafe)` because the value is constant after init —
  /// the dlsym lookup runs once and the pointer never changes — and
  /// the pointee is accessed under our own synchronisation discipline
  /// (only one `count(_:)` may be active at a time).
  nonisolated(unsafe) private static let loggerVar: UnsafeMutablePointer<MallocLogger?>? = {
    guard let handle = dlopen(nil, RTLD_LAZY),
      let sym = dlsym(handle, "malloc_logger")
    else { return nil }
    return sym.assumingMemoryBound(to: MallocLogger?.self)
  }()

  private static let counter = Atomic<UInt64>(0)
  /// Bit-pattern of `pthread_self()` for the thread currently being
  /// measured, or 0 when no measurement is active. Stored as an
  /// `Atomic<UInt>` so the malloc callback (which can fire from any
  /// thread) sees a consistent value.
  private static let watchedThreadBits = Atomic<UInt>(0)

  private static let mallocLogger: MallocLogger = { type, _, _, _, result, _ in
    guard type & allocBit != 0, result != 0 else { return }
    let watched = watchedThreadBits.load(ordering: .acquiring)
    guard watched != 0 else { return }
    let myBits = UInt(bitPattern: Int(bitPattern: pthread_self()))
    if myBits == watched {
      counter.wrappingAdd(1, ordering: .relaxed)
    }
  }

  /// Run `body` and return the number of heap allocation events
  /// observed on the calling thread during its execution. Returns `nil`
  /// when the platform's `malloc_logger` hook isn't reachable.
  ///
  /// Only one `count(_:)` invocation can be active in the process at a
  /// time; nesting is not supported (the inner call would overwrite
  /// the outer call's hook).
  static func count<R>(_ body: () throws -> R) rethrows -> (allocations: UInt64?, result: R) {
    guard let loggerVar = loggerVar else {
      let r = try body()
      return (nil, r)
    }
    let myBits = UInt(bitPattern: Int(bitPattern: pthread_self()))
    let prev = loggerVar.pointee
    counter.store(0, ordering: .relaxed)
    watchedThreadBits.store(myBits, ordering: .releasing)
    loggerVar.pointee = mallocLogger
    defer {
      loggerVar.pointee = prev
      watchedThreadBits.store(0, ordering: .releasing)
    }
    let r = try body()
    return (counter.load(ordering: .relaxed), r)
  }
}

// MARK: - Tests

@Suite(.serialized) struct HotPathAllocationTests {

  // MARK: - Resamplers

  @Test func Synchronous_Stereo() {
    let resampler = SynchronousResampler(
      channels: 2, inputRate: 44100, outputRate: 48000, chunkSize: 1024)
    runResamplerHotPath(resampler, channels: 2, label: "Synchronous stereo")
  }

  // MARK: - Filters

  @Test func Biquad_AllocationFree() {
    // Lowpass-ish coefficients. The hot path is the same regardless of
    // values; we just need a stable, non-zero filter so the inner loop
    // exercises the FMAs.
    let coeffs = BiquadCoefficients(b0: 0.25, b1: 0.5, b2: 0.25, a1: -0.5, a2: 0.1)
    let filter = BiquadFilter(coefficients: coeffs)
    let buffer = AudioBuffers(channels: 1, capacity: 1024)
    let wave = buffer[0]
    fillSine(wave, frames: 1024, freqHz: 1000, sampleRate: 44100)

    assertAllocationFree(label: "Biquad") { _ in
      filter.process(waveform: wave)
    }
  }

  @Test func Gain_AllocationFree() {
    var fp = GainParameters()
    fp.gain = -6.0
    fp.scale = .dB
    let filter = GainFilter(parameters: fp)
    let buffer = AudioBuffers(channels: 1, capacity: 1024)
    let wave = buffer[0]
    fillSine(wave, frames: 1024, freqHz: 1000, sampleRate: 44100)

    assertAllocationFree(label: "Gain") { _ in
      filter.process(waveform: wave)
    }
  }

  @Test func Volume_AllocationFree() {
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    params.targetVolume = -6.0
    params.isMuted = false
    let filter = VolumeFilter(processingParameters: params)
    let buffer = AudioBuffers(channels: 1, capacity: 1024)
    let wave = buffer[0]
    fillSine(wave, frames: 1024, freqHz: 1000, sampleRate: 44100)

    assertAllocationFree(label: "Volume") { _ in
      filter.process(waveform: wave)
    }
  }

  @Test func Loudness_AllocationFree() {
    var fp = LoudnessParameters()
    fp.referenceLevel = -25.0
    fp.highBoost = 10.0
    fp.lowBoost = 10.0
    fp.attenuateMid = false
    let filter = LoudnessFilter(
      parameters: fp,
      sampleRate: 44100)
    // Loudness needs a `processingParameters` reference, otherwise it
    // early-returns and the test doesn't exercise the inner biquads.
    let params = ProcessingParameters(captureChannels: 2, playbackChannels: 2)
    params.currentVolume = -45.0  // 20 dB below reference → full boost
    filter.processingParameters = params

    let buffer = AudioBuffers(channels: 1, capacity: 1024)
    let wave = buffer[0]
    fillSine(wave, frames: 1024, freqHz: 1000, sampleRate: 44100)

    assertAllocationFree(label: "Loudness") { _ in
      filter.process(waveform: wave)
    }
  }

  // MARK: - Mixer

  @Test func Mixer_2to4_AllocationFree() {
    let config = MixerConfig(
      channelsIn: 2, channelsOut: 4,
      mapping: [
        MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)]),
        MixerMapping(dest: 1, sources: [MixerSource(channel: 1, gain: 0.0)]),
        MixerMapping(
          dest: 2,
          sources: [
            MixerSource(channel: 0, gain: -3.0), MixerSource(channel: 1, gain: -3.0),
          ]),
        MixerMapping(dest: 3, sources: [MixerSource(channel: 1, gain: -6.0)]),
      ])
    let mixer = AudioMixer(config: config, chunkSize: 1024)

    let inputs = makeRandomChunks(count: 32, channels: 2, frames: 1024)
    var output = AudioChunk(
      waveforms: Array(repeating: [Double](repeating: 0, count: 1024), count: 4),
      validFrames: 0)

    let inputCount = inputs.count
    assertAllocationFree(label: "Mixer 2→4") { i in
      try! mixer.process(input: inputs[i % inputCount], into: &output)
    }
  }

  @Test func Logger_AllocationFree() {

    let logger = Logger(label: "test.alloc.free")
    let staticStr: StaticString = "Static string argument value"
    // 1. Assert logging with various arguments without dynamic string has no allocations
    assertAllocationFree(label: "Logger various arguments") { i in
      logger.info(
        "Test event: int=%d, float=%f, static=%s",
        .int(i),
        .double(3.14159 + Double(i)),
        .staticString(staticStr)
      )
    }

    // 2. Assert .string("\(Double(i) * 2.71828)") has allocations
    // To guarantee a heap allocation that bypasses Small String Optimization across iterations,
    // we ensure the string length exceeds 15 bytes by adding a descriptive prefix.
    let (allocationsWithString, _) = AllocationCounter.count {
      for i in 0..<30 {
        logger.info(
          "Test event: dynamic=%s",
          .string(
            "A sufficiently long dynamic string prefix to guarantee heap allocation: \(Double(i) * 2.71828)"
          )
        )
      }
    }
    if let n = allocationsWithString {
      print("[Logger with string] allocations=\(n) over 30 iterations")
      #if !DEBUG
        #expect(n > 0, "Logger operations with dynamic .string should trigger heap allocations")
      #endif
    }
  }

  // MARK: - ProcessingParameters

  @Test func ProcessingParameters_AllocationFree() {
    let params = ProcessingParameters()
    let chunks = makeRandomChunks(count: 32, channels: 2, frames: 1024)
    let chunkCount = chunks.count
    assertAllocationFree(label: "ProcessingParameters updateLevels") { i in
      _ = params.updateCaptureLevels(from: chunks[i % chunkCount])
    }
  }

  // MARK: - Helpers

  /// Standard hot-path harness: warm up, count allocations across the
  /// measurement window, print the result, and (in release) assert the
  /// count is below a small absolute bound.
  private func assertAllocationFree(
    label: String, warmup: Int = 0, iterations: Int = 30, body: (Int) -> Void
  ) {
    for i in 0..<warmup { body(i) }
    let (allocations, _) = AllocationCounter.count {
      for i in 0..<iterations { body(warmup + i) }
    }
    guard let n = allocations else {
      Issue.record("malloc_logger unavailable — \(label) skipped")
      return
    }
    print("[\(label)] allocations=\(n) over \(iterations) iterations")
    #if !DEBUG
      #expect(
        n < 10, "\(label) allocated \(n) times in steady state (expected ≈ 0)")
    #endif
  }

  private func runResamplerHotPath(
    _ resampler: AudioResampler, channels: Int, label: String
  ) {
    let cs = resampler.chunkSize
    let inputs = makeRandomChunks(count: 32, channels: channels, frames: cs)
    var output = AudioChunk(
      waveforms: Array(
        repeating: [Double](repeating: 0, count: resampler.maxOutputFrames),
        count: channels),
      validFrames: 0)
    let inputCount = inputs.count
    assertAllocationFree(label: label) { i in
      try! resampler.process(input: inputs[i % inputCount], into: &output)
    }
  }

  private func makeRandomChunks(
    count: Int, channels: Int, frames: Int, scale: Double = 1.0
  ) -> [AudioChunk] {
    var rng = SystemRandomNumberGenerator()
    return (0..<count).map { _ in
      let waveforms = (0..<channels).map { _ -> [Double] in
        (0..<frames).map { _ in Double.random(in: -scale...scale, using: &rng) }
      }
      return AudioChunk(waveforms: waveforms, validFrames: frames)
    }
  }

  private func fillSine(
    _ buf: MutableWaveform, frames: Int, freqHz: Double, sampleRate: Double
  ) {
    for i in 0..<frames {
      buf[i] = sin(2.0 * .pi * freqHz * Double(i) / sampleRate)
    }
  }
}
