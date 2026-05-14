import Accelerate
import DSPAudio
import DSPConfig
import Foundation

enum MixerError: Error, Sendable, CustomStringConvertible {
  /// `input.validFrames` is larger than the chunkSize the mixer was constructed with.
  case inputSizeMismatch(needed: Int, got: Int)
  /// Caller's output AudioChunk doesn't have enough capacity per channel.
  case outputBufferTooSmall(needed: Int, got: Int)
  /// Caller's output AudioChunk has the wrong channel count for this mixer.
  case channelCountMismatch(needed: Int, got: Int)

  var description: String {
    switch self {
    case .inputSizeMismatch(let needed, let got):
      return "Mixer input size mismatch: needed \(needed), got \(got)"
    case .outputBufferTooSmall(let needed, let got):
      return "Mixer output buffer too small: needed \(needed), got \(got)"
    case .channelCountMismatch(let needed, let got):
      return "Mixer channel count mismatch: needed \(needed), got \(got)"
    }
  }
}

/// Mixer that changes channel count and routes/sums audio between channels.
public final class AudioMixer {

  public let chunkSize: Int
  public let channelsIn: Int
  public let channelsOut: Int

  private struct PreparedSource {
    let inChannel: Int
    let gain: PrcFmt
  }
  private var mapping: [[PreparedSource]]

  public init(config: MixerConfig, chunkSize: Int) {
    self.chunkSize = chunkSize
    self.channelsIn = config.channelsIn
    self.channelsOut = config.channelsOut

    self.mapping = [[PreparedSource]](repeating: [], count: config.channelsOut)

    for map in config.mapping {
      let dest = map.dest
      guard dest < config.channelsOut else { continue }

      if map.mute == true {
        continue
      }

      var sources: [PreparedSource] = []
      for src in map.sources {
        if src.mute == true { continue }

        let gain = src.gainValue
        let isLinear = src.scale == .linear
        var linGain = isLinear ? gain : PrcFmt.fromDB(gain)

        if src.inverted == true {
          linGain *= -1.0
        }

        sources.append(PreparedSource(inChannel: src.channel, gain: linGain))
      }
      self.mapping[dest] = sources
    }
  }
  /// Zero-allocation API. The caller pre-allocates `output` with
  /// `output.channels == channelsOut` and `output.frames >= input.validFrames`.
  /// The mixer writes the mixed samples directly and sets `output.validFrames`.
  ///
  /// `input` and `output` must reference distinct buffers — the mixer
  /// accumulates into the output and reads input concurrently, so aliasing
  /// would corrupt the result.
  public func process(input: AudioChunk, into output: inout AudioChunk) throws {
    let frames = input.validFrames
    guard frames <= chunkSize else {
      throw MixerError.inputSizeMismatch(needed: chunkSize, got: frames)
    }
    guard output.channels == channelsOut else {
      throw MixerError.channelCountMismatch(needed: channelsOut, got: output.channels)
    }
    guard output.frames >= frames else {
      throw MixerError.outputBufferTooSmall(needed: frames, got: output.frames)
    }

    for outCh in 0..<channelsOut {
      let dst = output[outCh]

      if let base = dst.baseAddress {
        base.update(repeating: 0, count: frames)
      }

      let sources = mapping[outCh]
      for src in sources {
        guard src.inChannel < input.channels else { continue }
        let srcPtr = UnsafeBufferPointer(input[src.inChannel])

        if src.gain == 1.0 {
          DSPOps.add(srcPtr, dst, count: frames)
        } else if src.gain != 0.0 {
          DSPOps.multiplyAdd(srcPtr, src.gain, accumulator: dst, count: frames)
        }
      }
    }
    output.validFrames = frames
  }
}
