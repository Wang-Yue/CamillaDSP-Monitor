// Correctness tests for all in-tree resamplers (Synchronous, AsyncPoly, AsyncSinc):
//
//   * Per-channel state isolation — a stereo (2-channel) resampler
//     should produce the same per-channel output as two independent
//     mono resamplers, bit-for-bit.
//   * `process(input:into:)` (in-place API) should produce
//     identical output to the allocating `process(chunk:)` call.
//   * The in-place API should reject an output buffer that is
//     smaller than the resampler's `maxOutputFrames`.
//

import Foundation
import Testing

@testable import SwiftDSP
@testable import DSPConfig

@Suite struct ResamplerCorrectnessTests {

  // MARK: - Per-channel state isolation

  @Test func Stereo_MatchesPerChannelMono_Synchronous() throws {
    try assertStereoMatchesMono { channels, inR, outR, cs in
      SynchronousResampler(channels: channels, inputRate: inR, outputRate: outR, chunkSize: cs)
    }
  }

  @Test func Stereo_MatchesPerChannelMono_AsyncPoly() throws {
    try assertStereoMatchesMono { channels, inR, outR, cs in
      AsyncPolyResampler(
        channels: channels, inputRate: inR, outputRate: outR, interpolation: .cubic, chunkSize: cs)
    }
  }

  @Test func Stereo_MatchesPerChannelMono_AsyncSinc() throws {
    try assertStereoMatchesMono { channels, inR, outR, cs in
      AsyncSincResampler(
        channels: channels, inputRate: inR, outputRate: outR, profile: .accurate, chunkSize: cs)
    }
  }

  // MARK: - In-place API equivalence

  @Test func InoutAPI_Synchronous_MatchesAllocatingAPI() {
    assertInoutMatchesAlloc(
      makeA: {
        SynchronousResampler(channels: 2, inputRate: 44100, outputRate: 48000, chunkSize: 1024)
      },
      makeB: {
        SynchronousResampler(channels: 2, inputRate: 44100, outputRate: 48000, chunkSize: 1024)
      }
    )
  }

  @Test func InoutAPI_AsyncPoly_MatchesAllocatingAPI() {
    assertInoutMatchesAlloc(
      makeA: {
        AsyncPolyResampler(
          channels: 2, inputRate: 44100, outputRate: 48000, interpolation: .cubic, chunkSize: 1024)
      },
      makeB: {
        AsyncPolyResampler(
          channels: 2, inputRate: 44100, outputRate: 48000, interpolation: .cubic, chunkSize: 1024)
      }
    )
  }

  @Test func InoutAPI_AsyncSinc_MatchesAllocatingAPI() {
    assertInoutMatchesAlloc(
      makeA: {
        AsyncSincResampler(
          channels: 2, inputRate: 44100, outputRate: 48000, profile: .accurate, chunkSize: 1024)
      },
      makeB: {
        AsyncSincResampler(
          channels: 2, inputRate: 44100, outputRate: 48000, profile: .accurate, chunkSize: 1024)
      }
    )
  }

  // MARK: - Output buffer size validation

  @Test func InoutAPI_RejectsTooSmallOutputBuffer_Synchronous() {
    assertRejectsTooSmallOutputBuffer {
      SynchronousResampler(channels: 2, inputRate: 44100, outputRate: 48000, chunkSize: 1024)
    }
  }

  @Test func InoutAPI_RejectsTooSmallOutputBuffer_AsyncPoly() {
    assertRejectsTooSmallOutputBuffer {
      AsyncPolyResampler(
        channels: 2, inputRate: 44100, outputRate: 48000, interpolation: .cubic, chunkSize: 1024)
    }
  }

  @Test func InoutAPI_RejectsTooSmallOutputBuffer_AsyncSinc() {
    assertRejectsTooSmallOutputBuffer {
      AsyncSincResampler(
        channels: 2, inputRate: 44100, outputRate: 48000, profile: .accurate, chunkSize: 1024)
    }
  }

  // MARK: - Helpers

  private func assertStereoMatchesMono(
    factory: (Int, Int, Int, Int) -> AudioResampler
  ) throws {
    let inRate = 44100
    let outRate = 48000
    let chunkSize = 1024
    let nbrIn = 32 * chunkSize
    let left = makeSine(n: nbrIn, rate: inRate, freq: 1000.0)
    let right = makeSine(n: nbrIn, rate: inRate, freq: 1500.0)

    let stereo = factory(2, inRate, outRate, chunkSize)
    let monoL = factory(1, inRate, outRate, chunkSize)
    let monoR = factory(1, inRate, outRate, chunkSize)
    let cs = stereo.chunkSize

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
    #expect(stereoOutL.count == monoOutL.count)
    #expect(stereoOutR.count == monoOutR.count)
    var maxL = 0.0
    var maxR = 0.0
    for i in 0..<stereoOutL.count {
      maxL = max(maxL, abs(stereoOutL[i] - monoOutL[i]))
      maxR = max(maxR, abs(stereoOutR[i] - monoOutR[i]))
    }
    #expect(maxL == 0.0)
    #expect(maxR == 0.0)
  }

  private func assertInoutMatchesAlloc(
    makeA: () -> AudioResampler, makeB: () -> AudioResampler
  ) {
    let resamplerA = makeA()
    let resamplerB = makeB()
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

      #expect(outAlloc.validFrames == preallocated.validFrames)
      for ch in 0..<2 {
        for i in 0..<outAlloc.validFrames {
          #expect(abs(outAlloc.waveforms[ch][i] - preallocated.waveforms[ch][i]) <= 1e-12)
        }
      }
    }
  }

  private func assertRejectsTooSmallOutputBuffer(makeResampler: () -> AudioResampler) {
    let resampler = makeResampler()
    let inChunk = AudioChunk(
      waveforms: [[Double]](
        repeating: [Double](repeating: 0, count: resampler.chunkSize), count: 2),
      validFrames: resampler.chunkSize)
    var tooSmall = AudioChunk(
      waveforms: [[Double]](repeating: [Double](repeating: 0, count: 64), count: 2),
      validFrames: 0)
    do {
      try resampler.process(input: inChunk, into: &tooSmall)
      Issue.record("Expected outputBufferTooSmall error")
    } catch ResamplerError.outputBufferTooSmall {
      // expected
    } catch {
      Issue.record("Unexpected error: \(error)")
    }
  }

  private func makeSine(n: Int, rate: Int, freq: Double = 1000.0) -> [Double] {
    let omega = 2.0 * .pi * freq / Double(rate)
    return (0..<n).map { sin(omega * Double($0)) }
  }
}
