import Accelerate
import DSPAudio
import Foundation

public final class VolumeFilter: Filter {
  public var processingParameters: ProcessingParameters?

  public init(
    processingParameters: ProcessingParameters
  ) {
    self.processingParameters = processingParameters
  }

  public func process(waveform: MutableWaveform) {
    guard let params = processingParameters else { return }
    let targetVolume = params.targetVolume
    let mute = params.isMuted
    let gain = mute ? 0.0 : PrcFmt.fromDB(targetVolume)

    if gain == 1.0 {
      // No-op
    } else if gain == 0.0 {
      DSPOps.clear(waveform)
    } else {
      DSPOps.scalarMultiply(waveform, by: gain)
    }
  }
}
