// Test-only convenience APIs.
//
// The library exposes a strict zero-allocation `process(input:into:)` API on
// resamplers and the mixer. These extensions add an allocating
// `process(chunk:) -> AudioChunk` helper that is convenient for unit tests
// and one-shot scripts but has no place on the hot path. They live in the
// test target so the library never carries the allocation cost.

import CamillaDSPLib
import Foundation

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
