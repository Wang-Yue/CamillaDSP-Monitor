import Accelerate
import DSPAudio
import DSPConfig

final class GainFilter: Filter {
  private let linearGain: PrcFmt
  private let muted: Bool

  init(parameters: GainParameters) {
    self.muted = parameters.mute ?? false
    let gainValue = parameters.gain ?? 0.0
    var computedGain = parameters.scale == .linear ? gainValue : PrcFmt.fromDB(gainValue)

    if parameters.inverted == true {
      computedGain *= -1.0
    }

    self.linearGain = computedGain
  }

  func process(waveform: MutableWaveform) {
    if muted {
      DSPOps.clear(waveform)
    } else if linearGain != 1.0 {
      DSPOps.scalarMultiply(waveform, by: linearGain)
    }
  }
}
