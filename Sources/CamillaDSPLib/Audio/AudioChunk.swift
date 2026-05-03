// CamillaDSP-Swift: Core audio buffer type
// Non-interleaved float buffers, one vector per channel.

import Foundation

/// A chunk of non-interleaved audio data flowing through the pipeline.
public struct AudioChunk: Sendable {
  /// Number of frames (samples per channel) in this chunk
  public let frames: Int
  /// Number of channels
  public var channels: Int { waveforms.count }
  /// Peak positive sample value across all channels
  public var maxval: PrcFmt
  /// Peak negative sample value across all channels
  public var minval: PrcFmt
  /// When this chunk was created
  public let timestamp: Date
  /// Number of valid frames (may be < frames at end-of-stream)
  public var validFrames: Int
  /// Non-interleaved audio data: waveforms[channel][sample]
  public var waveforms: [[PrcFmt]]

  /// Create a new silent AudioChunk
  public init(frames: Int, channels: Int) {
    self.frames = frames
    self.maxval = 0.0
    self.minval = 0.0
    self.timestamp = Date()
    self.validFrames = frames
    self.waveforms = Array(repeating: Array(repeating: 0.0, count: frames), count: channels)
  }

  /// Create an AudioChunk from existing waveform data
  public init(waveforms: [[PrcFmt]], validFrames: Int? = nil) {
    let frames = waveforms.first?.count ?? 0
    self.frames = frames
    self.maxval = 0.0
    self.minval = 0.0
    self.timestamp = Date()
    self.validFrames = validFrames ?? frames
    self.waveforms = waveforms
    updatePeaks()
  }

  /// Update peak values from current waveform data
  public mutating func updatePeaks() {
    maxval = -.infinity
    minval = .infinity
    for ch in 0..<channels {
      for s in 0..<validFrames {
        let v = waveforms[ch][s]
        if v > maxval { maxval = v }
        if v < minval { minval = v }
      }
    }
  }

}
