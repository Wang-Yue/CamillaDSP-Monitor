import Accelerate
import DSPAudio
import DSPConfig

public final class GainFilter: Filter {
  public let name: String
  private var linearGain: PrcFmt
  private var muted: Bool

  public init(name: String = "gain", parameters: GainParameters) {
    self.name = name
    self.muted = parameters.mute ?? false
    let gainValue = parameters.gain ?? 0.0
    var computedGain = parameters.scale == .linear ? gainValue : PrcFmt.fromDB(gainValue)

    if parameters.inverted == true {
      computedGain *= -1.0
    }

    self.linearGain = computedGain
  }

  public func process(waveform: MutableWaveform) {
    if muted {
      DSPOps.clear(waveform)
    } else if linearGain != 1.0 {
      DSPOps.scalarMultiply(waveform, by: linearGain)
    }
  }

  public func processSingle(_ sample: PrcFmt) -> PrcFmt {
    if muted {
      return 0.0
    } else {
      return sample * linearGain
    }
  }
  public func updateParameters(_ config: FilterConfig, sampleRate: Int) {
    guard case .gain(let params) = config else { return }
    self.muted = params.mute ?? false
    let gainValue = params.gain ?? 0.0
    var computedGain = params.scale == .linear ? gainValue : PrcFmt.fromDB(gainValue)

    if params.inverted == true {
      computedGain *= -1.0
    }

    self.linearGain = computedGain
  }
}
