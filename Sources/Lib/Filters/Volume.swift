import Accelerate
import DSPAudio
import DSPConfig
import Foundation

public final class VolumeFilter: Filter {
  public let name: String
  private var fader: Fader
  private var volumeLimit: Double
  private let chunkSize: Int

  // Ramp state (tracks fader ramping)
  private var ramptimeInChunks: Int
  private var currentVolume: PrcFmt
  private var targetVolume: Double
  private var targetLinearGain: PrcFmt
  private var mute: Bool
  private var rampStart: PrcFmt
  private var rampStep: Int

  // Pre-allocated ramp gains for the current chunk to avoid heap allocation on the hot path
  private var currentRampGains: [PrcFmt]

  public var processingParameters: ProcessingParameters?

  public init(
    name: String = "volume",
    parameters: VolumeParameters = VolumeParameters(),
    sampleRate: Int,
    chunkSize: Int,
    processingParameters: ProcessingParameters
  ) {
    self.name = name
    self.fader = parameters.fader ?? .main
    let rampTimeMs = parameters.rampTime ?? 400.0
    self.volumeLimit = parameters.limit ?? 50.0
    self.chunkSize = chunkSize
    self.processingParameters = processingParameters

    self.ramptimeInChunks = Int(
      (rampTimeMs / (1000.0 * Double(chunkSize) / Double(sampleRate))).rounded())

    // Pre-allocate array
    self.currentRampGains = [PrcFmt](repeating: 0.0, count: chunkSize)

    // Initialize state from shared parameters to prevent volume burst on startup
    let initialVol = processingParameters.targetVolume(for: fader)
    let initialMute = processingParameters.isMuted(for: fader)
    let initialVolClamped = min(initialVol, volumeLimit)

    self.targetVolume = initialVolClamped
    self.mute = initialMute
    self.currentVolume = initialMute ? -100.0 : initialVolClamped
    self.targetLinearGain = initialMute ? 0.0 : PrcFmt.fromDB(initialVolClamped)
    self.rampStart = self.currentVolume
    self.rampStep = 0
  }

  /// Pre-calculates target volume levels and generates ramping array once per chunk.
  /// Must be called once per audio chunk before processing individual channel waveforms.
  public func prepareChunk() {
    guard let params = processingParameters else { return }

    let sharedVol = params.targetVolume(for: fader)
    let sharedMute = params.isMuted(for: fader)

    let targetVol = min(sharedVol, volumeLimit)

    if abs(targetVol - targetVolume) > 0.01 || mute != sharedMute {
      if ramptimeInChunks > 0 {
        rampStart = currentVolume
        rampStep = 1
      } else {
        currentVolume = sharedMute ? -100.0 : targetVol
        rampStep = 0
      }
      targetVolume = targetVol
      targetLinearGain = sharedMute ? 0.0 : PrcFmt.fromDB(targetVol)
      mute = sharedMute
    }

    if rampStep > 0 && rampStep <= ramptimeInChunks {
      fillRamp()
    }
  }

  /// Conforms to `Filter`. Processes a single channel's waveform slice.
  public func process(waveform: MutableWaveform) {
    let count = waveform.count
    guard count > 0 else { return }

    if rampStep == 0 {
      if targetLinearGain == 1.0 {
        // No-op
      } else if targetLinearGain == 0.0 {
        DSPOps.clear(waveform)
      } else {
        DSPOps.scalarMultiply(waveform, by: targetLinearGain)
      }
    } else {
      let limit = min(count, currentRampGains.count)
      DSPOps.multiply(currentRampGains, waveform, count: limit)
      if limit < count {
        let finalGain = mute ? 0.0 : PrcFmt.fromDB(targetVolume)
        let remainingWaveform = MutableWaveform(
          start: waveform.baseAddress?.advanced(by: limit), count: count - limit)
        DSPOps.scalarMultiply(remainingWaveform, by: finalGain)
      }
    }
  }

  /// Advances the fader's ramp steps.
  /// Must be called once per audio chunk after all channels have been processed.
  public func advanceRamp() {
    guard rampStep > 0 else { return }

    if currentRampGains.count > 0 {
      let lastGain = currentRampGains[min(chunkSize - 1, currentRampGains.count - 1)]
      currentVolume = 20.0 * log10(max(lastGain, 1e-150))
    }

    rampStep += 1
    if rampStep > ramptimeInChunks {
      rampStep = 0
    }

    processingParameters?.setCurrentVolume(currentVolume, for: fader)
  }

  private func fillRamp() {
    let targetVol: PrcFmt = mute ? -100.0 : PrcFmt(targetVolume)
    let ramprange = (targetVol - rampStart) / PrcFmt(ramptimeInChunks)
    let stepsize = ramprange / PrcFmt(chunkSize)

    for val in 0..<chunkSize {
      currentRampGains[val] = PrcFmt.fromDB(
        rampStart
          + ramprange * (PrcFmt(rampStep) - 1.0)
          + PrcFmt(val) * stepsize
      )
    }
  }

  public func updateParameters(_ config: FilterConfig, sampleRate: Int) {
    guard case .volume(let parameters) = config else { return }
    fader = parameters.fader ?? .main
    let rampTimeMs = parameters.rampTime ?? 400.0
    volumeLimit = parameters.limit ?? 50.0

    ramptimeInChunks = Int(
      (rampTimeMs / (1000.0 * Double(chunkSize) / Double(sampleRate))).rounded())

    if volumeLimit < currentVolume {
      currentVolume = volumeLimit
    }
  }
}
