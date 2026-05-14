import DSPAudio
import DSPConfig
import DSPFilters
import DSPMixer
import Foundation

enum PipelineError: Error, Sendable, CustomStringConvertible {
  case inputSizeMismatch(needed: Int, got: Int)
  case outputBufferTooSmall(needed: Int, got: Int)
  case channelCountMismatch(needed: Int, got: Int)

  var description: String {
    switch self {
    case .inputSizeMismatch(let needed, let got):
      return "Pipeline input size mismatch: needed \(needed), got \(got)"
    case .outputBufferTooSmall(let needed, let got):
      return "Pipeline output buffer too small: needed \(needed), got \(got)"
    case .channelCountMismatch(let needed, let got):
      return "Pipeline channel count mismatch: needed \(needed), got \(got)"
    }
  }
}

/// A single step in the processing pipeline
enum PipelineExecutionStep {
  /// Filter chain applied to a single channel
  case filter(channel: Int, filters: [Filter], bypassed: Bool)
  /// Mixer that changes channel routing.
  case mixer(AudioMixer)
}

/// The main audio processing pipeline.
public final class Pipeline {
  private var processingSteps: [PipelineExecutionStep] = []
  /// Implicit main volume filter with smooth ramping (matches Rust Pipeline.volume field)
  private let masterVolume: VolumeFilter
  /// Working scratch the pipeline copies the caller's input into at the start
  /// of each `process(...)`. With class-owned `AudioBuffers`, we can no
  /// longer rely on CoW to isolate mutations from the caller's `input`
  /// chunk — so we copy explicitly into this pre-allocated buffer.
  private var captureScratch: AudioChunk
  /// Pre-allocated scratch chunks mapped by the sequential step index in `steps` array
  /// to prevent Copy-On-Write allocations on the hot path.
  private var scratchesForMixers: [AudioChunk] = []

  private let framesPerChunk: Int
  private let rate: Int
  private let expectedInChannels: Int
  private let expectedOutChannels: Int

  public init(
    config: CamillaDSPConfig,
    processingParams: ProcessingParameters,
    explicitChunkSize: Int? = nil
  ) throws {
    self.framesPerChunk = explicitChunkSize ?? config.devices.chunksize
    self.rate = config.devices.samplerate
    // Create the implicit master volume filter — equivalent to the
    // `Pipeline.volume` slot in the Rust upstream (which keys off
    // fader index 0). Reads its initial state from the shared
    // `processingParameters` so the engine's pre-start
    // `setVolume`/`setMute` calls are honoured without a 0 dB ramp.
    self.masterVolume = VolumeFilter(processingParameters: processingParams)

    let inChannels = config.devices.capture.channels
    self.expectedInChannels = inChannels
    // Pre-allocate the input scratch sized for the capture-side channel count.

    self.captureScratch = AudioChunk(frames: framesPerChunk, channels: inChannels)

    // Track current channel count as we walk pipeline steps (matches Rust num_channels)
    var currentChannels = inChannels

    if let steps = config.pipeline {
      for step in steps {
        switch step.type {
        case .filter:
          guard let filterNames = step.names, !filterNames.isEmpty else {
            throw ConfigError.invalidPipeline("Filter step missing names")
          }
          let isBypassed = step.bypassed ?? false

          let channelsToApply: [Int]
          if let chs = step.channels {
            channelsToApply = chs
          } else if let ch = step.channel {
            channelsToApply = [ch]
          } else {
            channelsToApply = Array(0..<currentChannels)
          }

          // Create a separate filter chain for each target channel
          for ch in channelsToApply {
            var filters: [Filter] = []
            for name in filterNames {
              guard let filterConfig = config.filters?[name] else {
                throw ConfigError.invalidPipeline("Filter '\(name)' not defined")
              }
              let filter = try FilterFactory.create(
                config: filterConfig, sampleRate: rate, chunkSize: framesPerChunk)
              filters.append(filter)
            }
            processingSteps.append(.filter(channel: ch, filters: filters, bypassed: isBypassed))
          }

        case .mixer:
          guard let mixerName = step.name, let mixerConfig = config.mixers?[mixerName] else {
            throw ConfigError.invalidPipeline("Mixer step missing name or config")
          }
          let mixer = AudioMixer(config: mixerConfig, chunkSize: framesPerChunk)
          currentChannels = mixerConfig.channelsOut

          scratchesForMixers.append(AudioChunk(frames: framesPerChunk, channels: currentChannels))
          processingSteps.append(.mixer(mixer))
        }
      }
    }

    self.expectedOutChannels = currentChannels
  }

  public func process(input: AudioChunk, into output: inout AudioChunk) throws {
    let validFrames = input.validFrames
    // 1. Validate input and output buffer shapes/capacities against pipeline configurations.

    guard validFrames <= framesPerChunk else {
      throw PipelineError.inputSizeMismatch(needed: framesPerChunk, got: validFrames)
    }
    guard input.channels == expectedInChannels else {
      throw PipelineError.channelCountMismatch(needed: expectedInChannels, got: input.channels)
    }
    guard output.channels == expectedOutChannels else {
      throw PipelineError.channelCountMismatch(needed: expectedOutChannels, got: output.channels)
    }
    guard output.frames >= validFrames else {
      throw PipelineError.outputBufferTooSmall(needed: validFrames, got: output.frames)
    }
    // 2. Copy input into our pre-allocated scratch. The class-backed
    // `AudioBuffers` no longer shields the caller's chunk from in-place
    // mutation, so we make our own working copy up front.
    for ch in 0..<expectedInChannels {
      let src = input[ch]
      let dst = captureScratch[ch]
      if let srcBase = src.baseAddress, let dstBase = dst.baseAddress {
        dstBase.update(from: srcBase, count: validFrames)
      }
    }
    captureScratch.validFrames = validFrames

    var currentChunk = captureScratch
    // 3. Implicit main volume with smooth ramp (matches Rust volume filter).
    // Mutates workingChunk's samples in place.
    for ch in 0..<currentChunk.channels {
      let buf = currentChunk[ch]
      let slice = UnsafeMutableBufferPointer(start: buf.baseAddress, count: validFrames)
      masterVolume.process(waveform: slice)
    }

    // 4. Execute pipeline steps sequentially.
    var mixerIdx = 0

    for step in processingSteps {
      switch step {
      case .filter(let ch, let filters, let bypassed):
        if bypassed { continue }
        guard ch < currentChunk.channels else { continue }
        let buf = currentChunk[ch]
        let slice = UnsafeMutableBufferPointer(start: buf.baseAddress, count: validFrames)
        for filter in filters {
          filter.process(waveform: slice)
        }

      case .mixer(let mixer):
        var scratch = scratchesForMixers[mixerIdx]
        try mixer.process(input: currentChunk, into: &scratch)
        currentChunk = scratch
        mixerIdx += 1
      }
    }

    // 5. Copy the final computed samples from workingChunk to caller-supplied output buffer.
    output.validFrames = validFrames
    for ch in 0..<expectedOutChannels {
      let src = currentChunk[ch]
      let dst = output[ch]
      if let srcBase = src.baseAddress, let dstBase = dst.baseAddress {
        dstBase.update(from: srcBase, count: validFrames)
      }
    }
  }
}
