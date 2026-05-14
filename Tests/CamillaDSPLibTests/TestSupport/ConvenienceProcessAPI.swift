// Test-only convenience APIs.
//
// The library exposes a strict zero-allocation `process(input:into:)` API on
// resamplers and the mixer. These extensions add an allocating
// `process(chunk:) -> AudioChunk` helper that is convenient for unit tests
// and one-shot scripts but has no place on the hot path. They live in the
// test target so the library never carries the allocation cost.

import Foundation

@testable import DSPAudio
@testable import DSPFilters
@testable import DSPMixer
@testable import DSPResampler

extension Filter {
  /// Test-only adapter — the library API takes a buffer pointer (no CoW),
  /// but tests find `[PrcFmt]` literals more convenient. The `inout` form
  /// here trades the realtime guarantees for ergonomics; never use it on
  /// the audio thread.
  func process(waveform: inout [PrcFmt]) {
    waveform.withUnsafeMutableBufferPointer { ptr in
      process(waveform: ptr)
    }
  }
}

extension AudioResampler {
  /// Allocates a fresh output AudioChunk sized for the worst-case ratio
  /// (`maxOutputFrames`) and dispatches to `process(input:into:)`. Slicing
  /// down to `validFrames` is the caller's responsibility.
  func process(chunk: AudioChunk) throws -> AudioChunk {
    var output = AudioChunk(
      waveforms: (0..<channels).map { _ in [Double](repeating: 0, count: maxOutputFrames) },
      validFrames: 0)
    try process(input: chunk, into: &output)
    return output
  }
}

extension AudioMixer {
  func process(chunk: AudioChunk) -> AudioChunk {
    let validFrames = chunk.validFrames
    var output = AudioChunk(
      waveforms: (0..<channelsOut).map { _ in [Double](repeating: 0, count: validFrames) },
      validFrames: 0)
    // Allocated buffer can't alias `chunk`; sizes match by construction.
    try! process(input: chunk, into: &output)
    return output
  }
}

// MARK: - Convenience conversions and snapshot methods for testing

extension AudioBuffers {
  /// Convenience init that copies an existing `[[PrcFmt]]` into a fresh
  /// pool. `capacity` defaults to the longest input channel; shorter
  /// channels are zero-padded.
  public convenience init(copying waveforms: [[PrcFmt]]) {
    let chCount = waveforms.count
    let cap = waveforms.map { $0.count }.max() ?? 0
    self.init(channels: max(chCount, 1), capacity: max(cap, 1))
    for ch in 0..<chCount {
      let src = waveforms[ch]
      let dst = channelBuffers[ch]
      for i in 0..<src.count {
        dst[i] = src[i]
      }
    }
  }

  /// Snapshot a single channel's first `count` samples as an `Array`.
  /// Convenience for tests/debug; not for hot-path use.
  public func snapshotChannel(_ ch: Int, count: Int? = nil) -> [PrcFmt] {
    let n = count ?? capacity
    precondition(n <= capacity, "snapshot count exceeds capacity")
    let buf = channelBuffers[ch]
    return Array(UnsafeBufferPointer(start: buf.baseAddress, count: n))
  }
}

extension AudioChunk {
  /// Create an AudioChunk from existing waveform data. Copies samples
  /// into a fresh `AudioBuffers`. Used by tests and one-shot helpers —
  /// not on the audio thread.
  public init(waveforms: [[PrcFmt]], validFrames: Int? = nil) {
    let buffers = AudioBuffers(copying: waveforms)
    self.init(buffers: buffers, validFrames: validFrames)
  }

  /// Read-only `[[PrcFmt]]` snapshot of the entire (capacity-sized) sample
  /// storage. Allocates fresh `Array`s on every call — strictly for tests
  /// and debug. Never use on the audio thread; the hot path should access
  /// samples via `chunk[ch]` (an `UnsafeMutableBufferPointer`).
  public var waveforms: [[PrcFmt]] {
    (0..<channels).map { ch in buffers.snapshotChannel(ch) }
  }
}
