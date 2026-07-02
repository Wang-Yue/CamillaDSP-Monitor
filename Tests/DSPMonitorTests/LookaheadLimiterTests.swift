import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters
@testable import DSPProcessors

@Suite struct LookaheadLimiterTests {
  private static func isClose(_ left: PrcFmt, _ right: PrcFmt, maxdiff: PrcFmt) -> Bool {
    return abs(left - right) < maxdiff
  }

  private static func compareWaveforms(_ left: [PrcFmt], _ right: [PrcFmt], maxdiff: PrcFmt) -> Bool
  {
    guard left.count == right.count else { return false }
    for (val_l, val_r) in zip(left, right) {
      if !isClose(val_l, val_r, maxdiff: maxdiff) {
        return false
      }
    }
    return true
  }

  @Test func test_lookahead_limiter_basic() {
    let params = LookaheadLimiterParameters(
      limit: 0.0,
      attack: 4.0,
      release: 1.0 / log(2.0),
      unit: .samples
    )
    let filter = LookaheadLimiterFilter(parameters: params, sampleRate: 48000, chunkSize: 1024)

    var input: [PrcFmt] = [
      1.0, 1.0, 1.0, 1.0, 1.0, 2.0, -2.0, 1.0, 1.0, 2.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
      1.0, 1.0,
    ]
    let expected: [PrcFmt] = [
      0.0,
      0.0,
      0.0,
      0.0,
      1.0,
      1.0,
      0.875,
      0.75,
      0.625,
      1.0,
      -1.0,
      pow(0.5, 1.0 / 2.0),
      0.625,
      1.0,
      pow(0.5, 1.0 / 2.0),
      pow(0.5, 1.0 / 4.0),
      pow(0.5, 1.0 / 8.0),
      pow(0.5, 1.0 / 16.0),
      pow(0.5, 1.0 / 32.0),
    ]

    filter.process(waveform: &input)
    #expect(Self.compareWaveforms(input, expected, maxdiff: 1e-6))
  }

  @Test func test_lookahead_limiter_same_as_limiter() {
    let paramsLookahead = LookaheadLimiterParameters(
      limit: 0.0,
      attack: 0.0,
      release: 0.0,
      unit: .samples
    )
    let filterLookahead = LookaheadLimiterFilter(
      parameters: paramsLookahead, sampleRate: 48000, chunkSize: 1024)

    let paramsLimiter = LimiterParameters(clipLimit: 0.0, softClip: false)
    let filterLimiter = LimiterFilter(parameters: paramsLimiter)

    var lookaheadInput: [PrcFmt] = [0.5, 1.0, 2.0, -2.0, -1.0, -0.5, 1.5, -1.5, 0.0]
    var limiterInput = lookaheadInput

    filterLookahead.process(waveform: &lookaheadInput)
    filterLimiter.process(waveform: &limiterInput)

    #expect(lookaheadInput == limiterInput)
  }

  @Test func test_lookahead_limiter_zero_attack_matches_compressor() {
    let releaseSamples: PrcFmt = 4.0
    let samplerate = 48000
    let limiterInput: [PrcFmt] = [2.0, 1.0, 1.0, 1.0, 1.0]
    let chunksize = limiterInput.count

    let configLim = LookaheadLimiterParameters(
      limit: 0.0,
      attack: 0.0,
      release: releaseSamples,
      unit: .samples
    )
    let limiter = LookaheadLimiterFilter(
      parameters: configLim, sampleRate: samplerate, chunkSize: chunksize)

    let configComp = CompressorParameters(
      channels: 1,
      monitorChannels: nil,
      processChannels: nil,
      attack: 0.0,
      release: Double(releaseSamples) / Double(samplerate),
      threshold: 0.0,
      factor: 1e20,
      makeupGain: nil,
      softClip: nil,
      clipLimit: nil
    )
    let compressor = CompressorProcessor(
      parameters: configComp, sampleRate: samplerate, chunkSize: chunksize)

    var compressorChunk = AudioChunk(frames: chunksize, channels: 1)
    for i in 0..<chunksize {
      compressorChunk[0][i] = limiterInput[i]
    }

    var limiterWave = limiterInput
    limiter.process(waveform: &limiterWave)

    try! compressor.process(chunk: &compressorChunk)

    #expect(
      Self.compareWaveforms(
        limiterWave, Array(compressorChunk[0][0..<chunksize]), maxdiff: 1e-6))
  }

  @Test func test_lookahead_limiter_zero_release() {
    let params = LookaheadLimiterParameters(
      limit: 0.0,
      attack: 2.0,
      release: 0.0,
      unit: .samples
    )
    let filter = LookaheadLimiterFilter(parameters: params, sampleRate: 48000, chunkSize: 1024)
    var input: [PrcFmt] = [1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0]

    filter.process(waveform: &input)

    for val in input {
      #expect(abs(val) <= 1.0)
    }
  }

  @Test func test_lookahead_limiter_state_persistence() {
    let params = LookaheadLimiterParameters(
      limit: 0.0,
      attack: 5.0,
      release: 1.0 / log(2.0),
      unit: .samples
    )
    let filter = LookaheadLimiterFilter(parameters: params, sampleRate: 48000, chunkSize: 1024)

    var buf1: [PrcFmt] = [1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 1.0, 1.0, 1.0, 1.0, 1.0]
    let expected1: [PrcFmt] = [0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.9, 0.8, 0.7, 0.6, 1.0]
    filter.process(waveform: &buf1)
    #expect(Self.compareWaveforms(buf1, expected1, maxdiff: 1e-6))

    var buf2: [PrcFmt] = [1.0, 1.0, 1.0, 1.0]
    let expected2: [PrcFmt] = [
      pow(0.5, 1.0 / 2.0),
      pow(0.5, 1.0 / 4.0),
      pow(0.5, 1.0 / 8.0),
      pow(0.5, 1.0 / 16.0),
    ]
    filter.process(waveform: &buf2)
    #expect(Self.compareWaveforms(buf2, expected2, maxdiff: 1e-6))
  }

  @Test func test_lookahead_limiter_attack_over_one_second_rejected() {
    let params = LookaheadLimiterParameters(
      limit: 0.0,
      attack: 48001.0,
      release: 4.0,
      unit: .samples
    )
    #expect(throws: Error.self) {
      try params.validate(sampleRate: 48000)
    }
  }

  @Test func test_lookahead_limiter_chunksize_larger_than_samplerate() {
    let samplerate = 4
    let chunksize = 8
    let params = LookaheadLimiterParameters(
      limit: 0.0,
      attack: 4.0,
      release: 1.0,
      unit: .samples
    )
    let filter = LookaheadLimiterFilter(
      parameters: params, sampleRate: samplerate, chunkSize: chunksize)
    var input: [PrcFmt] = [1.0, 1.0, 2.0, 1.0, 1.0, -2.0, 1.0, 1.0]

    filter.process(waveform: &input)

    #expect(input.count == chunksize)
  }
}
