// Volume filter — chunk-granular tracking of the master volume control.
// Reads `targetVolume` / `isMuted` from `ProcessingParameters` once per
// chunk; on a change, applies the gain immediately.
// After processing, writes back the `currentVolume` so any other filter
// that needs to observe the effective volume (e.g. `LoudnessFilter`)
// sees the latest value.

import Accelerate
import Foundation

public final class VolumeFilter: Filter {
  public let name: String
  private var volumeLimit: Double
  private let sampleRate: Int
  private let chunkSize: Int

  private var currentVolume: PrcFmt  // dB
  private var targetVolume: Double  // dB target (clamped to limit)
  private var targetLinearGain: PrcFmt  // 10^(targetVolume/20), or 0 if muted
  private var mute: Bool

  /// Backing store for the user-facing target volume + mute. Set by
  /// `Pipeline.init` for the implicit master-volume slot.
  public var processingParameters: ProcessingParameters?

  public init(name: String, config: FilterConfig, sampleRate: Int, chunkSize: Int) {
    self.name = name
    self.volumeLimit = config.parameters.limit ?? 50.0
    self.sampleRate = sampleRate
    self.chunkSize = chunkSize

    self.currentVolume = 0.0
    self.targetVolume = 0.0
    self.targetLinearGain = 1.0
    self.mute = false
  }

  /// Direct-construction initialiser used by `Pipeline` for the
  /// implicit master volume and by tests.
  public init(
    name: String, rampTimeMs: Double, limit: Double, currentVolume: Double,
    mute: Bool, chunkSize: Int, sampleRate: Int,
    processingParameters: ProcessingParameters
  ) {
    self.name = name
    self.volumeLimit = limit
    self.sampleRate = sampleRate
    self.chunkSize = chunkSize
    self.processingParameters = processingParameters

    let currentVolumeWithMute: PrcFmt = mute ? -100.0 : currentVolume
    self.currentVolume = currentVolumeWithMute
    self.targetVolume = currentVolume
    self.mute = mute
    self.targetLinearGain = mute ? 0.0 : pow(10.0, currentVolume / 20.0)
  }

  /// Sync target volume + mute from the shared parameters.
  private func prepareProcessing() {
    guard let params = processingParameters else { return }
    let sharedVol = params.targetVolume
    let sharedMute = params.isMuted

    // Clamp to the configured limit before comparing.
    let targetVol = min(sharedVol, volumeLimit)

    if abs(targetVol - targetVolume) > 0.01 || mute != sharedMute {
      targetVolume = targetVol
      targetLinearGain = sharedMute ? 0.0 : pow(10.0, PrcFmt(targetVol) / 20.0)
      mute = sharedMute
      currentVolume = sharedMute ? -100.0 : PrcFmt(targetVol)
    }
  }

  public func process(waveform: inout [PrcFmt]) throws {
    prepareProcessing()

    if targetLinearGain == 1.0 {
      // No gain change needed
    } else if targetLinearGain == 0.0 {
      waveform.withUnsafeMutableBufferPointer { ptr in
        vDSP.clear(&ptr)
      }
    } else {
      waveform.withUnsafeMutableBufferPointer { ptr in
        vDSP.multiply(targetLinearGain, ptr, result: &ptr)
      }
    }

    processingParameters?.currentVolume = currentVolume
  }

  public func updateParameters(_ config: FilterConfig) {
    volumeLimit = config.parameters.limit ?? 50.0
    if volumeLimit < currentVolume {
      currentVolume = volumeLimit
      targetVolume = volumeLimit
      targetLinearGain = pow(10.0, currentVolume / 20.0)
    }
  }
}
