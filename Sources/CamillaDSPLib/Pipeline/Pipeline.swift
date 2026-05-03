// CamillaDSP-Swift: Processing pipeline - sequential chain of filters, mixers, and processors

import Foundation
import Logging

/// A single step in the processing pipeline
public enum PipelineExecutionStep {
  /// Filter chain applied to a single channel
  case filter(channel: Int, filters: [Filter], bypassed: Bool)
  /// Mixer that changes channel routing. The associated `scratchIdx` is an
  /// index into `Pipeline.mixerScratches`, the array of pre-allocated swap
  /// buffers (one per mixer step, sized exactly for that mixer's channel
  /// count and the pipeline's chunk size).
  case mixer(AudioMixer, scratchIdx: Int)
}

/// The main audio processing pipeline
public final class Pipeline {
  private let logger = Logger(label: "camilladsp.pipeline")
  private var steps: [PipelineExecutionStep]
  private let processingParams: ProcessingParameters
  private let sampleRate: Int
  private let chunkSize: Int

  /// Implicit main volume filter with smooth ramping (matches Rust Pipeline.volume field)
  private var volumeFilter: VolumeFilter

  /// One pre-allocated scratch chunk per mixer step, sized exactly for that
  /// mixer's `channelsOut` and the pipeline's fixed `chunkSize`. Indexed via
  /// `PipelineExecutionStep.mixer(_, scratchIdx:)`. No dynamic resize, no
  /// allocation on the hot path — `process(chunk:)` just swaps with the
  /// appropriate pre-built scratch.
  private var mixerScratches: [AudioChunk]

  // Performance monitoring
  private var loadHistory: [Double] = []
  private var overloadCount: Int = 0

  public init(
    config: CamillaDSPConfig,
    processingParams: ProcessingParameters,
    explicitChunkSize: Int? = nil
  ) throws {
    self.processingParams = processingParams
    self.sampleRate = config.devices.samplerate
    self.chunkSize = explicitChunkSize ?? config.devices.chunksize
    self.steps = []

    // Create the implicit master volume filter — equivalent to the
    // `Pipeline.volume` slot in the Rust upstream (which keys off
    // fader index 0). Reads its initial state from the shared
    // `processingParameters` so the engine's pre-start
    // `setVolume`/`setMute` calls are honoured without a 0 dB ramp.
    let rampTimeMs = config.devices.volumeRampTime ?? 200.0
    let volumeLimit = config.devices.volumeLimit ?? 50.0
    self.volumeFilter = VolumeFilter(
      name: "default",
      rampTimeMs: rampTimeMs,
      limit: volumeLimit,
      currentVolume: processingParams.currentVolume,
      mute: processingParams.isMuted,
      chunkSize: config.devices.chunksize,
      sampleRate: config.devices.samplerate,
      processingParameters: processingParams
    )

    // Pre-allocate one scratch chunk per mixer step. The chunkSize is fixed
    // at construction, and each mixer's `channelsOut` is known from its
    // config — so we can size every scratch buffer exactly once and never
    // resize on the hot path.
    self.mixerScratches = []

    guard let pipelineSteps = config.pipeline else {
      logger.info("No pipeline defined, passthrough mode")
      return
    }

    // Track current channel count as we walk pipeline steps (matches Rust num_channels)
    var numChannels = config.devices.capture.channels

    for step in pipelineSteps {
      switch step.type {
      case .filter:
        guard let names = step.names, !names.isEmpty else {
          throw ConfigError.invalidPipeline("Filter step missing names")
        }
        let bypassed = step.bypassed ?? false

        // Determine which channels this filter step applies to.
        // Rust behavior: if channels is Some, use that list; otherwise apply to 0..num_channels.
        let targetChannels: [Int]
        if let channels = step.channels {
          targetChannels = channels
        } else if let channel = step.channel {
          targetChannels = [channel]
        } else {
          // No explicit channel list: apply to all current channels (matches Rust 0..num_channels)
          targetChannels = Array(0..<numChannels)
          logger.debug("Filter step \(names) applied to all \(numChannels) channels")
        }

        // Create a separate filter chain for each target channel
        for ch in targetChannels {
          var filters: [Filter] = []
          for name in names {
            guard let filterConfig = config.filters?[name] else {
              throw ConfigError.invalidPipeline("Filter '\(name)' not defined")
            }
            let filter = try FilterFactory.create(
              name: "\(name)_ch\(ch)", config: filterConfig,
              sampleRate: sampleRate, chunkSize: chunkSize
            )
            if let vol = filter as? VolumeFilter {
              vol.processingParameters = processingParams
            } else if let loud = filter as? LoudnessFilter {
              loud.processingParameters = processingParams
            }
            filters.append(filter)
          }
          steps.append(.filter(channel: ch, filters: filters, bypassed: bypassed))
        }

      case .mixer:
        guard let name = step.name, let mixerConfig = config.mixers?[name] else {
          throw ConfigError.invalidPipeline("Mixer step missing name or config")
        }
        try MixerValidator.validate(mixerConfig)
        let mixer = AudioMixer(name: name, config: mixerConfig, chunkSize: chunkSize)
        // Allocate a scratch sized exactly for this mixer's output channel
        // count and the pipeline's chunkSize. The index is baked into the
        // step so the hot path can swap directly with no lookup.
        let scratch = AudioChunk(frames: chunkSize, channels: mixerConfig.channelsOut)
        let scratchIdx = mixerScratches.count
        mixerScratches.append(scratch)
        // Update tracked channel count to mixer output (matches Rust: num_channels = mixconf.channels.out)
        numChannels = mixerConfig.channelsOut
        steps.append(.mixer(mixer, scratchIdx: scratchIdx))
      }
    }

    logger.info("Pipeline initialized with \(steps.count) steps")
  }

  /// Process an audio chunk through the entire pipeline
  public func process(chunk: inout AudioChunk) throws {
    let startTime = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)

    // Implicit main volume with smooth ramp (matches Rust: self.volume.process_chunk(&mut chunk))
    // The VolumeFilter reads target volume and mute from processingParams internally
    // and applies a smooth per-sample ramp -- no clicks on volume changes.
    for ch in 0..<chunk.channels {
      try volumeFilter.process(waveform: &chunk.waveforms[ch])
    }

    for step in steps {
      switch step {
      case .filter(let channel, let filters, let bypassed):
        if bypassed { continue }
        guard channel < chunk.channels else { continue }
        for filter in filters {
          try filter.process(waveform: &chunk.waveforms[channel])
        }

      case .mixer(let mixer, let scratchIdx):
        // The scratch was sized for this mixer's channelsOut × chunkSize at
        // pipeline init. Mix into it, then swap so `chunk` carries the mixed
        // output and the scratch recycles the now-discarded input buffers.
        try mixer.process(input: chunk, into: &mixerScratches[scratchIdx])
        swap(&chunk, &mixerScratches[scratchIdx])
      }
    }

    // Measure processing load
    let elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - startTime
    let chunkDuration = Double(chunkSize) / Double(sampleRate) * 1_000_000_000.0
    let load = Double(elapsed) / chunkDuration * 100.0
    processingParams.processingLoad = load

    if load > 100.0 {
      overloadCount += 1
      if overloadCount >= 10 {
        logger.warning(
          "Processing overload: \(String(format: "%.1f", load))% for 10+ consecutive chunks")
      }
    } else {
      overloadCount = 0
    }
  }

}
