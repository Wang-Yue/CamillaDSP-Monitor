import Accelerate
import DSPAudio
import DSPConfig
import Foundation

public final class LimiterFilter: Filter {
  public let name: String
  private var clipLimit: PrcFmt
  private var softClip: Bool

  public init(name: String = "limiter", parameters: LimiterParameters) {
    self.name = name
    self.clipLimit = PrcFmt.fromDB(parameters.clipLimit)
    self.softClip = parameters.softClip ?? false
  }

  public func process(waveform: MutableWaveform) {
    if softClip {
      for i in 0..<waveform.count {
        var scaled = waveform[i] / clipLimit
        scaled = max(-1.5, min(1.5, scaled))
        waveform[i] = (scaled - (scaled * scaled * scaled) / 6.75) * clipLimit
      }
    } else {
      var lowLimit = -clipLimit
      var highLimit = clipLimit
      guard let base = waveform.baseAddress else { return }
      vDSP_vclipD(base, 1, &lowLimit, &highLimit, base, 1, vDSP_Length(waveform.count))
    }
  }
  public func updateParameters(_ config: FilterConfig, sampleRate: Int) {
    guard case .limiter(let params) = config else { return }
    self.clipLimit = PrcFmt.fromDB(params.clipLimit)
    self.softClip = params.softClip ?? false
  }
}
