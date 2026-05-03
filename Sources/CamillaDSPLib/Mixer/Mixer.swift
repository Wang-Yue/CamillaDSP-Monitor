// CamillaDSP-Swift: Mixer - routes and sums audio between channels.
//
// The mixer is rate-preserving (output frames == input frames) and runs on a
// fixed `chunkSize` known at init. There is no internal scratch and no
// dynamic allocation on the hot path — `process(input:into:)` writes directly
// to the caller-supplied output buffer.

import Accelerate
import Foundation

/// Mixer config validation
public enum MixerValidator {
  public static func validate(_ config: MixerConfig) throws {
    var seenDests = Set<Int>()
    for map in config.mapping {
      guard map.dest < config.channelsOut else {
        throw ConfigError.invalidFilter(
          "mixer dest \(map.dest) >= channels_out \(config.channelsOut)")
      }
      guard !seenDests.contains(map.dest) else {
        throw ConfigError.invalidFilter("mixer dest \(map.dest) mapped more than once")
      }
      seenDests.insert(map.dest)

      var seenSources = Set<Int>()
      for src in map.sources {
        guard src.channel < config.channelsIn else {
          throw ConfigError.invalidFilter(
            "mixer source channel \(src.channel) >= channels_in \(config.channelsIn)")
        }
        guard !seenSources.contains(src.channel) else {
          throw ConfigError.invalidFilter(
            "mixer source channel \(src.channel) listed more than once for dest \(map.dest)")
        }
        seenSources.insert(src.channel)
      }
    }
  }
}

/// A mixer source: one input channel contributing to an output channel
public struct MixerSourceEntry {
  public let channel: Int
  public let gain: PrcFmt
}

public enum MixerError: Error {
  /// `input.validFrames` is larger than the chunkSize the mixer was constructed with.
  case inputSizeMismatch(needed: Int, got: Int)
  /// Caller's output AudioChunk doesn't have enough capacity per channel.
  case outputBufferTooSmall(needed: Int, got: Int)
  /// Caller's output AudioChunk has the wrong channel count for this mixer.
  case channelCountMismatch(needed: Int, got: Int)
}

/// Mixer that changes channel count and routes/sums audio between channels.
public final class AudioMixer {
  public let name: String
  public let chunkSize: Int
  public private(set) var channelsIn: Int
  public private(set) var channelsOut: Int
  private var mapping: [[MixerSourceEntry]]  // mapping[outCh] = list of sources
  private var mutedOutputs: Set<Int>

  public init(name: String, config: MixerConfig, chunkSize: Int) {
    precondition(chunkSize > 0, "chunkSize must be positive")
    self.name = name
    self.chunkSize = chunkSize
    self.channelsIn = config.channelsIn
    self.channelsOut = config.channelsOut
    self.mapping = []
    self.mutedOutputs = []
    applyConfig(config)
  }

  public func updateParameters(_ config: MixerConfig) {
    applyConfig(config)
  }

  private func applyConfig(_ config: MixerConfig) {
    channelsIn = config.channelsIn
    channelsOut = config.channelsOut

    var newMapping = [[MixerSourceEntry]](repeating: [], count: config.channelsOut)
    var newMutedOutputs = Set<Int>()

    for map in config.mapping {
      if map.mute == true {
        newMutedOutputs.insert(map.dest)
        continue
      }

      var sources: [MixerSourceEntry] = []
      for src in map.sources {
        if src.mute == true { continue }

        var linearGain: PrcFmt
        switch src.scale ?? .dB {
        case .dB:
          linearGain = PrcFmt.fromDB(src.gainValue)
        case .linear:
          linearGain = src.gainValue
        }
        if src.inverted == true { linearGain *= -1.0 }

        sources.append(MixerSourceEntry(channel: src.channel, gain: linearGain))
      }
      newMapping[map.dest] = sources
    }

    mapping = newMapping
    mutedOutputs = newMutedOutputs
  }

  /// Output frames per call (== input frames; the mixer is rate-preserving).
  public func expectedOutputFrames(forInputFrames inputFrames: Int) -> Int { inputFrames }

  /// Zero-allocation API. The caller pre-allocates `output` with
  /// `output.waveforms.count == channelsOut` and each
  /// `output.waveforms[ch].count >= input.validFrames`. The mixer writes the
  /// mixed samples directly and sets `output.validFrames`.
  ///
  /// `input` and `output` must reference distinct buffers — the mixer
  /// accumulates into the output and reads input concurrently, so aliasing
  /// would corrupt the result.
  public func process(input: AudioChunk, into output: inout AudioChunk) throws {
    let validFrames = input.validFrames
    if validFrames > chunkSize {
      throw MixerError.inputSizeMismatch(needed: chunkSize, got: validFrames)
    }
    if output.waveforms.count != channelsOut {
      throw MixerError.channelCountMismatch(
        needed: channelsOut, got: output.waveforms.count)
    }
    for outCh in 0..<channelsOut {
      if output.waveforms[outCh].count < validFrames {
        throw MixerError.outputBufferTooSmall(
          needed: validFrames, got: output.waveforms[outCh].count)
      }
    }

    // Zero only the prefix we're about to populate (caller's buffer may be
    // longer than validFrames; we leave the trailing slop untouched).
    for outCh in 0..<channelsOut {
      output.waveforms[outCh].withUnsafeMutableBufferPointer { ptr in
        guard let base = ptr.baseAddress else { return }
        base.update(repeating: 0, count: validFrames)
      }
    }

    // Accumulate sources into output in place.
    for outCh in 0..<channelsOut {
      if mutedOutputs.contains(outCh) { continue }
      let sources = mapping[outCh]
      for source in sources {
        guard source.channel < input.channels else { continue }
        let inputWaveform = input.waveforms[source.channel]
        if source.gain == 1.0 {
          DSPOps.add(inputWaveform, &output.waveforms[outCh], count: validFrames)
        } else {
          DSPOps.multiplyAdd(
            inputWaveform, source.gain,
            accumulator: &output.waveforms[outCh], count: validFrames)
        }
      }
    }
    output.validFrames = validFrames
  }
}
