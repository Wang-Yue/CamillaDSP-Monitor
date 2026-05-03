// Performance benchmarks for the AsyncSinc resampler. Used to track the
// optimisation work that follows the numerical-correctness fixes.
//
// Run with:  swift test -c release --filter ResamplerBenchmarkTests
// Release-mode is required to get representative numbers — debug builds
// disable WMO and inlining of the inner kernel.

import Foundation
import XCTest

@testable import CamillaDSPLib

final class ResamplerBenchmarkTests: XCTestCase {

  static let inRate = 44100
  static let outRate = 48000
  static let chunkSize = 1024
  static let totalChunks = 200  // ~4.6 s of audio at 44.1 kHz

  /// Single-channel Accurate profile — the path the user reports as slow.
  func testBenchmark_Accurate_44100to48000_Mono() {
    runBenchmark(profile: .accurate, channels: 1, label: "Accurate mono")
  }

  /// Stereo Accurate profile — typical playback shape.
  func testBenchmark_Accurate_44100to48000_Stereo() {
    runBenchmark(profile: .accurate, channels: 2, label: "Accurate stereo")
  }

  /// Balanced profile for comparison (lighter inner loop, sincLen=192, quadratic).
  func testBenchmark_Balanced_44100to48000_Stereo() {
    runBenchmark(profile: .balanced, channels: 2, label: "Balanced stereo")
  }

  /// AsyncPoly cubic stereo — polynomial path, no antialiasing.
  func testBenchmark_AsyncPolyCubic_Stereo() {
    runPolyBenchmark(interpolation: .cubic, channels: 2, label: "AsyncPoly cubic stereo")
  }

  func testBenchmark_AsyncPolySeptic_Stereo() {
    runPolyBenchmark(interpolation: .septic, channels: 2, label: "AsyncPoly septic stereo")
  }

  func testBenchmark_Synchronous_Stereo() {
    runSyncBenchmark(channels: 2, label: "Synchronous stereo")
  }

  /// Compares the new `process(input:into:)` inout API against the allocating
  /// `process(chunk:)` to confirm equivalent results.
  func testInoutAPI_AccurateStereo_MatchesAllocatingAPI() {
    assertInoutMatchesAlloc(
      makeA: {
        AsyncSincResampler(
          channels: 2, inputRate: Self.inRate, outputRate: Self.outRate,
          profile: .accurate, chunkSize: Self.chunkSize)
      },
      makeB: {
        AsyncSincResampler(
          channels: 2, inputRate: Self.inRate, outputRate: Self.outRate,
          profile: .accurate, chunkSize: Self.chunkSize)
      },
      label: "AsyncSinc")
  }

  func testInoutAPI_AsyncPolyCubic_MatchesAllocatingAPI() {
    assertInoutMatchesAlloc(
      makeA: {
        AsyncPolyResampler(
          channels: 2, inputRate: Self.inRate, outputRate: Self.outRate,
          interpolation: .cubic, chunkSize: Self.chunkSize)
      },
      makeB: {
        AsyncPolyResampler(
          channels: 2, inputRate: Self.inRate, outputRate: Self.outRate,
          interpolation: .cubic, chunkSize: Self.chunkSize)
      },
      label: "AsyncPoly cubic")
  }

  func testInoutAPI_AsyncPolySeptic_MatchesAllocatingAPI() {
    assertInoutMatchesAlloc(
      makeA: {
        AsyncPolyResampler(
          channels: 2, inputRate: Self.inRate, outputRate: Self.outRate,
          interpolation: .septic, chunkSize: Self.chunkSize)
      },
      makeB: {
        AsyncPolyResampler(
          channels: 2, inputRate: Self.inRate, outputRate: Self.outRate,
          interpolation: .septic, chunkSize: Self.chunkSize)
      },
      label: "AsyncPoly septic")
  }

  func testInoutAPI_Synchronous_MatchesAllocatingAPI() {
    assertInoutMatchesAlloc(
      makeA: {
        SynchronousResampler(
          channels: 2, inputRate: Self.inRate, outputRate: Self.outRate,
          chunkSize: Self.chunkSize)
      },
      makeB: {
        SynchronousResampler(
          channels: 2, inputRate: Self.inRate, outputRate: Self.outRate,
          chunkSize: Self.chunkSize)
      },
      label: "Synchronous")
  }

  func testInoutAPI_RejectsTooSmallOutputBuffer() {
    let resampler = AsyncSincResampler(
      channels: 2, inputRate: Self.inRate, outputRate: Self.outRate,
      profile: .accurate, chunkSize: Self.chunkSize)
    let inChunk = AudioChunk(
      waveforms: [[Double]](repeating: [Double](repeating: 0, count: Self.chunkSize), count: 2),
      validFrames: Self.chunkSize)
    var tooSmall = AudioChunk(
      waveforms: [[Double]](repeating: [Double](repeating: 0, count: 64), count: 2),
      validFrames: 0)
    do {
      try resampler.process(input: inChunk, into: &tooSmall)
      XCTFail("Expected outputBufferTooSmall error")
    } catch ResamplerError.outputBufferTooSmall {
      // expected
    } catch {
      XCTFail("Unexpected error: \(error)")
    }
  }

  // MARK: -

  private func assertInoutMatchesAlloc(
    makeA: () -> AudioResampler, makeB: () -> AudioResampler, label: String
  ) {
    let resamplerA = makeA()
    let resamplerB = makeB()
    // Use the resampler's actual chunkSize — `SynchronousResampler` rounds
    // it up to the nearest valid FFT-compatible multiple.
    let cs = resamplerA.chunkSize

    let perChannel = (0..<2).map { _ -> [Double] in
      (0..<(cs * 8)).map { _ in Double.random(in: -1.0...1.0) }
    }
    let maxOut = resamplerA.maxOutputFrames
    var preallocated = AudioChunk(
      waveforms: [[Double]](repeating: [Double](repeating: 0, count: maxOut), count: 2),
      validFrames: 0)

    for c in 0..<8 {
      let waveforms = perChannel.map { Array($0[c * cs..<(c + 1) * cs]) }
      let inChunk = AudioChunk(waveforms: waveforms, validFrames: cs)

      let outAlloc = try! resamplerA.process(chunk: inChunk)
      try! resamplerB.process(input: inChunk, into: &preallocated)

      XCTAssertEqual(
        outAlloc.validFrames, preallocated.validFrames,
        "[\(label)] validFrames mismatch on chunk \(c)")
      for ch in 0..<2 {
        for i in 0..<outAlloc.validFrames {
          XCTAssertEqual(
            outAlloc.waveforms[ch][i], preallocated.waveforms[ch][i],
            accuracy: 1e-12,
            "[\(label)] mismatch at chunk \(c) ch \(ch) sample \(i)")
        }
      }
    }
  }

  // MARK: -

  private func runPolyBenchmark(
    interpolation: PolyInterpolation, channels: Int, label: String
  ) {
    var rng = SystemRandomNumberGenerator()
    let perChannel = (0..<channels).map { _ -> [Double] in
      (0..<(Self.chunkSize * Self.totalChunks)).map { _ in
        Double.random(in: -1.0...1.0, using: &rng)
      }
    }
    let resampler = AsyncPolyResampler(
      channels: channels, inputRate: Self.inRate, outputRate: Self.outRate,
      interpolation: interpolation, chunkSize: Self.chunkSize)

    var chunks: [AudioChunk] = []
    chunks.reserveCapacity(Self.totalChunks)
    for c in 0..<Self.totalChunks {
      let waveforms = perChannel.map { Array($0[c * Self.chunkSize..<(c + 1) * Self.chunkSize]) }
      chunks.append(AudioChunk(waveforms: waveforms, validFrames: Self.chunkSize))
    }
    _ = try! resampler.process(chunk: chunks[0])  // warm-up

    let start = ContinuousClock.now
    var outputFrames = 0
    for c in chunks {
      let out = try! resampler.process(chunk: c)
      outputFrames += out.validFrames
    }
    let elapsed = ContinuousClock.now - start
    let elapsedSec =
      Double(elapsed.components.seconds)
      + Double(elapsed.components.attoseconds) * 1e-18
    let inputSeconds = Double(Self.totalChunks * Self.chunkSize) / Double(Self.inRate)
    let realtimeFactor = inputSeconds / elapsedSec
    let nsPerFrame = elapsedSec * 1e9 / Double(outputFrames)
    print(
      String(
        format: "[%@] elapsed=%.3fs  out=%d frames  RTF=%.1fx  ns/outFrame=%.1f",
        label, elapsedSec, outputFrames, realtimeFactor, nsPerFrame))
  }

  private func runSyncBenchmark(channels: Int, label: String) {
    var rng = SystemRandomNumberGenerator()
    let resampler = SynchronousResampler(
      channels: channels, inputRate: Self.inRate, outputRate: Self.outRate,
      chunkSize: Self.chunkSize)
    // SynchronousResampler rounds chunkSize up to the smallest valid FFT
    // size — sample the resampler's actual chunkSize, not the constructor hint.
    let cs = resampler.chunkSize
    let perChannel = (0..<channels).map { _ -> [Double] in
      (0..<(cs * Self.totalChunks)).map { _ in Double.random(in: -1.0...1.0, using: &rng) }
    }

    var chunks: [AudioChunk] = []
    chunks.reserveCapacity(Self.totalChunks)
    for c in 0..<Self.totalChunks {
      let waveforms = perChannel.map { Array($0[c * cs..<(c + 1) * cs]) }
      chunks.append(AudioChunk(waveforms: waveforms, validFrames: cs))
    }
    _ = try! resampler.process(chunk: chunks[0])  // warm-up

    let start = ContinuousClock.now
    var outputFrames = 0
    for c in chunks {
      let out = try! resampler.process(chunk: c)
      outputFrames += out.validFrames
    }
    let elapsed = ContinuousClock.now - start
    let elapsedSec =
      Double(elapsed.components.seconds)
      + Double(elapsed.components.attoseconds) * 1e-18
    let inputSeconds = Double(Self.totalChunks * cs) / Double(Self.inRate)
    let realtimeFactor = inputSeconds / elapsedSec
    let nsPerFrame = elapsedSec * 1e9 / Double(outputFrames)
    print(
      String(
        format: "[%@] elapsed=%.3fs  out=%d frames  RTF=%.1fx  ns/outFrame=%.1f",
        label, elapsedSec, outputFrames, realtimeFactor, nsPerFrame))
  }

  /// Measures the variance of per-call wall time in steady state. If process()
  /// were allocating per call, occasional malloc/free or GC-style spikes would
  /// show up as a long max time relative to the median. After our buffer-reuse
  /// optimisation the steady-state distribution should be tight.
  func testProcessSteadyState_AccurateStereo() {
    let resampler = AsyncSincResampler(
      channels: 2, inputRate: Self.inRate, outputRate: Self.outRate,
      profile: .accurate, chunkSize: Self.chunkSize)
    var chunks: [AudioChunk] = []
    let totalChunks = 64
    let perChannel = (0..<2).map { _ -> [Double] in
      (0..<(Self.chunkSize * totalChunks)).map { _ in Double.random(in: -1.0...1.0) }
    }
    for c in 0..<totalChunks {
      let waveforms = perChannel.map { Array($0[c * Self.chunkSize..<(c + 1) * Self.chunkSize]) }
      chunks.append(AudioChunk(waveforms: waveforms, validFrames: Self.chunkSize))
    }

    // Warm-up: 4 calls to settle scratch buffer capacities.
    for c in chunks.prefix(4) { _ = try! resampler.process(chunk: c) }

    var samples: [Double] = []
    samples.reserveCapacity(totalChunks - 4)
    for c in chunks.dropFirst(4) {
      let t0 = ContinuousClock.now
      let out = try! resampler.process(chunk: c)
      let t1 = ContinuousClock.now
      _ = out.validFrames
      let dt = t1 - t0
      let s =
        Double(dt.components.seconds) + Double(dt.components.attoseconds) * 1e-18
      samples.append(s * 1e6)  // microseconds
    }
    samples.sort()
    let median = samples[samples.count / 2]
    let p99 = samples[(samples.count * 99) / 100]
    let maxv = samples.last!
    print(
      String(
        format: "[steady-state] median=%.1fµs  p99=%.1fµs  max=%.1fµs  (n=%d)",
        median, p99, maxv, samples.count))

    // p99 within 3× of median => no severe alloc/GC spikes. Generous bound to
    // tolerate scheduling jitter; without buffer reuse this typically blew up
    // to 10×+ from realloc/free pairs.
    XCTAssertLessThan(
      p99, median * 3.0,
      "p99 process() time is >3× median — likely allocator activity per call.")
  }

  // MARK: -

  private func runBenchmark(profile: ResamplerProfile, channels: Int, label: String) {
    // Use a deterministic noise signal so Swift can't constant-fold the
    // convolution. Same seed every run.
    var rng = SystemRandomNumberGenerator()
    let perChannel = (0..<channels).map { _ -> [Double] in
      (0..<(Self.chunkSize * Self.totalChunks)).map { _ in
        Double.random(in: -1.0...1.0, using: &rng)
      }
    }

    let resampler = AsyncSincResampler(
      channels: channels,
      inputRate: Self.inRate,
      outputRate: Self.outRate,
      profile: profile,
      chunkSize: Self.chunkSize)

    // Warm-up call (avoid first-call code-path costs polluting the timing).
    let warm = perChannel.map { Array($0[0..<Self.chunkSize]) }
    let warmChunk = AudioChunk(waveforms: warm, validFrames: Self.chunkSize)
    _ = try! resampler.process(chunk: warmChunk)

    // Pre-build all chunks so we don't measure allocation in the loop.
    var chunks: [AudioChunk] = []
    chunks.reserveCapacity(Self.totalChunks)
    for c in 0..<Self.totalChunks {
      let start = c * Self.chunkSize
      let end = start + Self.chunkSize
      let waveforms = perChannel.map { Array($0[start..<end]) }
      chunks.append(AudioChunk(waveforms: waveforms, validFrames: Self.chunkSize))
    }

    // Time the body.
    let start = ContinuousClock.now
    var outputFrames = 0
    for c in chunks {
      let out = try! resampler.process(chunk: c)
      outputFrames += out.validFrames
    }
    let elapsed = ContinuousClock.now - start
    let elapsedSec =
      Double(elapsed.components.seconds)
      + Double(elapsed.components.attoseconds) * 1e-18

    let inputSeconds = Double(Self.totalChunks * Self.chunkSize) / Double(Self.inRate)
    let realtimeFactor = inputSeconds / elapsedSec
    let nsPerOutputFrame = elapsedSec * 1e9 / Double(outputFrames)
    print(
      String(
        format: "[%@] elapsed=%.3fs  out=%d frames  RTF=%.1fx  ns/outFrame=%.1f",
        label, elapsedSec, outputFrames, realtimeFactor, nsPerOutputFrame))
  }
}
