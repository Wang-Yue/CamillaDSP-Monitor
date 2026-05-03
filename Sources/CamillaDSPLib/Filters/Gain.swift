// CamillaDSP-Swift: Gain filter - simple amplitude scaling

import Accelerate

public final class GainFilter: Filter {
  public let name: String
  private var linearGain: PrcFmt
  private var muted: Bool

  public init(name: String, config: FilterConfig) {
    self.name = name
    let params = config.parameters
    let gain = params.gain ?? 0.0
    let inverted = params.inverted ?? false
    self.muted = params.mute ?? false

    switch params.scale ?? .dB {
    case .dB:
      self.linearGain = PrcFmt.fromDB(gain)
    case .linear:
      self.linearGain = gain
    }

    if inverted {
      self.linearGain *= -1.0
    }
  }

  public func process(waveform: inout [PrcFmt]) throws {
    if muted {
      waveform.withUnsafeMutableBufferPointer { ptr in
        vDSP.clear(&ptr)
      }
      return
    }
    DSPOps.scalarMultiply(&waveform, by: linearGain)
  }

  public func updateParameters(_ config: FilterConfig) {
    let params = config.parameters
    let gain = params.gain ?? 0.0
    let inverted = params.inverted ?? false
    self.muted = params.mute ?? false

    switch params.scale ?? .dB {
    case .dB:
      self.linearGain = PrcFmt.fromDB(gain)
    case .linear:
      self.linearGain = gain
    }
    if inverted { self.linearGain *= -1.0 }
  }
}
