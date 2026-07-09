import Accelerate
import DSPConfig
import Foundation

final class LimiterFilter: Filter {
  let name: String
  private var clipLimit: Double
  private var softClip: Bool

  init(name: String = "limiter", parameters: LimiterParameters) {
    self.name = name
    self.clipLimit = Double.fromDB(parameters.clipLimit)
    self.softClip = parameters.softClip ?? false
  }

  func process(waveform: MutableWaveform) {
    if softClip {
      guard let base = waveform.baseAddress else { return }
      let count = waveform.count
      let invLimit = 1.0 / clipLimit
      for i in 0..<count {
        var scaled = base[i] * invLimit
        scaled = max(-1.5, min(1.5, scaled))
        base[i] = (scaled - (scaled * scaled * scaled) / 6.75) * clipLimit
      }
    } else {
      var lowLimit = -clipLimit
      var highLimit = clipLimit
      guard let base = waveform.baseAddress else { return }
      vDSP_vclipD(base, 1, &lowLimit, &highLimit, base, 1, vDSP_Length(waveform.count))
    }
  }
}
