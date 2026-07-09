import Accelerate
import DSPConfig
import Foundation

final class VolumeFilter: Filter {
  let name: String
  private var fader: Fader
  private var volumeLimit: Double
  private let chunkSize: Int

  // Ramp state (tracks fader ramping)
  private var ramptimeInChunks: Int
  private var staleRampThresholdNs: UInt64
  private var currentVolume: Double
  private var targetVolume: Double
  private var targetLinearGain: Double
  private var mute: Bool
  private var rampStart: Double
  private var rampStep: Int

  // Pre-allocated ramp gains for the current chunk to avoid heap allocation on the hot path
  private var currentRampGains: UnsafeMutablePointer<Double>

  var processingParameters: ProcessingParameters?

  init(
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
    self.staleRampThresholdNs = UInt64(1_500_000_000) * UInt64(chunkSize) / UInt64(sampleRate)

    // Pre-allocate array
    self.currentRampGains = .allocate(capacity: chunkSize)
    self.currentRampGains.initialize(repeating: 0.0, count: chunkSize)

    // Initialize state from shared parameters to prevent volume burst on startup
    let initialVol = processingParameters.targetVolume(for: fader)
    let initialMute = processingParameters.isMuted(for: fader)
    let initialVolClamped = min(initialVol, volumeLimit)

    self.targetVolume = initialVolClamped
    self.mute = initialMute
    self.currentVolume = initialMute ? -100.0 : initialVolClamped
    self.targetLinearGain = initialMute ? 0.0 : Double.fromDB(initialVolClamped)
    self.rampStart = self.currentVolume
    self.rampStep = 0
  }

  deinit {
    currentRampGains.deinitialize(count: chunkSize)
    currentRampGains.deallocate()
  }

  /// Pre-calculates target volume levels and generates ramping array once per chunk.
  /// Must be called once per audio chunk before processing individual channel waveforms.
  func prepareChunk() {
    guard let params = processingParameters else { return }

    let sharedVol = params.targetVolume(for: fader)
    let sharedMute = params.isMuted(for: fader)

    let targetVol = min(sharedVol, volumeLimit)

    if abs(targetVol - targetVolume) > 0.01 || mute != sharedMute {
      let setAt = params.targetVolumeSetAt(for: fader)
      let now = DispatchTime.now().uptimeNanoseconds
      let rampIsStale = now > setAt ? ((now - setAt) > staleRampThresholdNs) : false

      if ramptimeInChunks > 0 && !rampIsStale {
        rampStart = currentVolume
        rampStep = 1
      } else {
        currentVolume = sharedMute ? -100.0 : targetVol
        rampStep = 0
      }
      targetVolume = targetVol
      targetLinearGain = sharedMute ? 0.0 : Double.fromDB(targetVol)
      mute = sharedMute
    }

    if rampStep > 0 && rampStep <= ramptimeInChunks {
      fillRamp()
    }
  }

  /// Conforms to `Filter`. Processes a single channel's waveform slice.
  func process(waveform: MutableWaveform) {
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
      let limit = min(count, chunkSize)
      DSPOps.multiply(UnsafePointer(currentRampGains), waveform, count: limit)
      if limit < count {
        let finalGain = mute ? 0.0 : Double.fromDB(targetVolume)
        let remainingWaveform = MutableWaveform(
          start: waveform.baseAddress?.advanced(by: limit), count: count - limit)
        DSPOps.scalarMultiply(remainingWaveform, by: finalGain)
      }
    }
  }

  /// Advances the fader's ramp steps.
  /// Must be called once per audio chunk after all channels have been processed.
  func advanceRamp() {
    guard rampStep > 0 else { return }

    if chunkSize > 0 {
      let lastGain = currentRampGains[chunkSize - 1]
      currentVolume = 20.0 * log10(max(lastGain, 1e-150))
    }

    rampStep += 1
    if rampStep > ramptimeInChunks {
      rampStep = 0
    }

    processingParameters?.setCurrentVolume(currentVolume, for: fader)
  }

  private func fillRamp() {
    let targetVol: Double = mute ? -100.0 : Double(targetVolume)
    let ramprange = (targetVol - rampStart) / Double(ramptimeInChunks)
    let stepsize = ramprange / Double(chunkSize)

    for val in 0..<chunkSize {
      currentRampGains[val] = Double.fromDB(
        rampStart
          + ramprange * (Double(rampStep) - 1.0)
          + Double(val) * stepsize
      )
    }
  }

  func updateParameters(_ config: FilterConfig, sampleRate: Int) {
    guard case .volume(let parameters) = config else { return }
    fader = parameters.fader ?? .main
    let rampTimeMs = parameters.rampTime ?? 400.0
    volumeLimit = parameters.limit ?? 50.0

    ramptimeInChunks = Int(
      (rampTimeMs / (1000.0 * Double(chunkSize) / Double(sampleRate))).rounded())
    staleRampThresholdNs = UInt64(1_500_000_000) * UInt64(chunkSize) / UInt64(sampleRate)

    if volumeLimit < currentVolume {
      currentVolume = volumeLimit
    }
  }

  func transferState(from src: Filter) {
    guard let srcVol = src as? VolumeFilter else { return }
    self.currentVolume = srcVol.currentVolume
    self.targetVolume = srcVol.targetVolume
    self.targetLinearGain = srcVol.targetLinearGain
    self.mute = srcVol.mute
    self.rampStart = srcVol.rampStart
    self.rampStep = srcVol.rampStep
    if self.chunkSize == srcVol.chunkSize {
      self.currentRampGains.initialize(from: srcVol.currentRampGains, count: chunkSize)
    }
  }
}
