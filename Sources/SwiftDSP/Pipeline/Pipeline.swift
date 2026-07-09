import DSPConfig
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

/// The execution mode of the audio processing pipeline.
public enum ProcessingMode: Sendable {
  case singleThreaded
  case multiThreaded
}

/// A filter chain applied to a single channel in parallel.
struct ParallelFilterChain: @unchecked Sendable {
  let channel: Int
  let filters: [Filter]
  let bypassed: Bool
}

/// A single stage in the processing pipeline.
enum PipelineExecutionStage {
  /// Contiguous filter chains that can be processed in parallel.
  case parallelFilters([ParallelFilterChain])
  /// Mixer that changes channel routing.
  case mixer(AudioMixer)
  /// Audio processor applied to the chunk in-place.
  case processor(Processor, bypassed: Bool)
}

/// The main audio processing pipeline.
final class Pipeline {
  private var processingStages: [PipelineExecutionStage] = []
  private let mode: ProcessingMode
  /// Implicit main volume filter with smooth ramping
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

  init(
    config: DSPConfiguration,
    processingParams: ProcessingParameters,
    explicitChunkSize: Int? = nil,
    mode: ProcessingMode = .singleThreaded
  ) throws {
    self.mode = mode
    self.framesPerChunk = explicitChunkSize ?? config.devices.chunksize
    self.rate = config.devices.samplerate
    // Create the implicit master volume filter — equivalent to the
    // master volume slot (which keys off
    // fader index 0). Reads its initial state from the shared
    // `processingParameters` so the engine's pre-start
    // `setVolume`/`setMute` calls are honoured without a 0 dB ramp.
    // Read the volume ramp time and safety limits from the devices configuration.
    let volumeRampTime = config.devices.volumeRampTime ?? 400.0
    let volumeLimit = config.devices.volumeLimit ?? 50.0

    self.masterVolume = VolumeFilter(
      parameters: VolumeParameters(rampTime: volumeRampTime, limit: volumeLimit, fader: .main),
      sampleRate: rate,
      chunkSize: framesPerChunk,
      processingParameters: processingParams
    )

    let inChannels = config.devices.capture.channels ?? 0
    self.expectedInChannels = inChannels
    // Pre-allocate the input scratch sized for the capture-side channel count.

    self.captureScratch = AudioChunk(frames: framesPerChunk, channels: inChannels)

    // Track current channel count as we walk pipeline steps
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

          var newChains: [ParallelFilterChain] = []
          // Create a separate filter chain for each target channel
          for ch in channelsToApply {
            var filters: [Filter] = []
            for name in filterNames {
              guard let filterConfig = config.filters?[name] else {
                throw ConfigError.invalidPipeline("Filter '\(name)' not defined")
              }
              let filter = try FilterFactory.create(
                name: name,
                config: filterConfig,
                sampleRate: rate,
                chunkSize: framesPerChunk,
                processingParameters: processingParams
              )
              filters.append(filter)
            }
            newChains.append(
              ParallelFilterChain(channel: ch, filters: filters, bypassed: isBypassed))
          }

          // Merge adjacent filter steps if they target disjoint sets of channels
          if !processingStages.isEmpty,
            case .parallelFilters(let existingChains) = processingStages.last!
          {
            let existingChannels = Set(existingChains.map { $0.channel })
            let newChannels = Set(newChains.map { $0.channel })
            if existingChannels.isDisjoint(with: newChannels) {
              processingStages[processingStages.count - 1] = .parallelFilters(
                existingChains + newChains)
            } else {
              processingStages.append(.parallelFilters(newChains))
            }
          } else {
            processingStages.append(.parallelFilters(newChains))
          }

        case .mixer:
          guard let mixerName = step.name, let mixerConfig = config.mixers?[mixerName] else {
            throw ConfigError.invalidPipeline("Mixer step missing name or config")
          }
          let mixer = AudioMixer(name: mixerName, config: mixerConfig, chunkSize: framesPerChunk)
          currentChannels = mixerConfig.channelsOut

          scratchesForMixers.append(AudioChunk(frames: framesPerChunk, channels: currentChannels))
          processingStages.append(.mixer(mixer))

        case .processor:
          guard let processorName = step.name,
            let processorConfig = config.processors?[processorName]
          else {
            throw ConfigError.invalidPipeline("Processor step missing name or config")
          }
          let isBypassed = step.bypassed ?? false
          let processor = try ProcessorFactory.create(
            name: processorName,
            config: processorConfig,
            sampleRate: rate,
            chunkSize: framesPerChunk
          )
          processingStages.append(
            .processor(processor, bypassed: isBypassed))
        }
      }
    }

    self.expectedOutChannels = currentChannels
  }

  func process(input: AudioChunk, into output: inout AudioChunk) throws {
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
    // 3. Implicit main volume with smooth ramp.
    // Mutates workingChunk's samples in place.
    masterVolume.prepareChunk()
    for ch in 0..<currentChunk.channels {
      let buf = currentChunk[ch]
      let slice = UnsafeMutableBufferPointer(start: buf.baseAddress, count: validFrames)
      masterVolume.process(waveform: slice)
    }
    masterVolume.advanceRamp()

    // 4. Execute pipeline stages.
    var mixerIdx = 0

    for stage in processingStages {
      switch stage {
      case .parallelFilters(let chains):
        let chunk = currentChunk
        if mode == .multiThreaded && chains.count > 1 {
          DispatchQueue.concurrentPerform(iterations: chains.count) { idx in
            let chain = chains[idx]
            if chain.bypassed { return }
            guard chain.channel < chunk.channels else { return }
            let buf = chunk[chain.channel]
            let slice = UnsafeMutableBufferPointer(start: buf.baseAddress, count: validFrames)
            for j in 0..<chain.filters.count {
              chain.filters[j].process(waveform: slice)
            }
          }
        } else {
          for chain in chains {
            if chain.bypassed { continue }
            guard chain.channel < chunk.channels else { continue }
            let buf = chunk[chain.channel]
            let slice = UnsafeMutableBufferPointer(start: buf.baseAddress, count: validFrames)
            for j in 0..<chain.filters.count {
              chain.filters[j].process(waveform: slice)
            }
          }
        }

      case .mixer(let mixer):
        var scratch = scratchesForMixers[mixerIdx]
        try mixer.process(input: currentChunk, into: &scratch)
        currentChunk = scratch
        mixerIdx += 1

      case .processor(let processor, let bypassed):
        if bypassed { continue }
        try processor.process(chunk: &currentChunk)
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

  func transferState(from src: Pipeline) {
    // 1. Transfer master volume
    self.masterVolume.transferState(from: src.masterVolume)

    // 2. Transfer stages
    for destStage in self.processingStages {
      switch destStage {
      case .parallelFilters(let destChains):
        for destChain in destChains {
          // Find matching filter chain in src by channel
          for srcStage in src.processingStages {
            if case .parallelFilters(let srcChains) = srcStage {
              if let srcChain = srcChains.first(where: { $0.channel == destChain.channel }) {
                // Match individual filters by name
                for destF in destChain.filters {
                  if let srcF = srcChain.filters.first(where: { $0.name == destF.name }) {
                    destF.transferState(from: srcF)
                  }
                }
              }
            }
          }
        }
      case .processor(let destProc, _):
        // Find matching processor step in src by name
        for srcStage in src.processingStages {
          if case .processor(let srcProc, _) = srcStage, srcProc.name == destProc.name {
            destProc.transferState(from: srcProc)
            break
          }
        }
      case .mixer:
        break
      }
    }
  }
}
