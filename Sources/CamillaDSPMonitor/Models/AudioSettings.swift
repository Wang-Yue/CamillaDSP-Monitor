// AudioSettings - Processing parameters and user preferences

import Foundation
import Observation

public enum ResamplerType: String, Codable, Sendable, CaseIterable, Identifiable {
  case asyncSinc = "AsyncSinc"
  case asyncPoly = "AsyncPoly"
  case synchronous = "Synchronous"
  public var id: String { rawValue }
}

public enum ResamplerProfile: String, Codable, Sendable, CaseIterable, Identifiable {
  case veryFast = "VeryFast"
  case fast = "Fast"
  case balanced = "Balanced"
  case accurate = "Accurate"
  public var id: String { rawValue }
}

public enum ResamplerInterpolation: String, Codable, Sendable, CaseIterable, Identifiable {
  case linear = "Linear"
  case quadratic = "Quadratic"
  case cubic = "Cubic"
  case sinc = "Sinc"
  public var id: String { rawValue }
}

@MainActor
@Observable
final class AudioSettings {
  let defaults = UserDefaults.standard

  var chunkSize: Int = 1024 {
    didSet {
      defaults.set(chunkSize, forKey: "chunksize")
      onChanged?()
    }
  }
  var enableRateAdjust: Bool = false {
    didSet {
      defaults.set(enableRateAdjust, forKey: "enableRateAdjust")
      onChanged?()
    }
  }
  var resamplerEnabled: Bool = false {
    didSet {
      defaults.set(resamplerEnabled, forKey: "resamplerEnabled")
      onChanged?()
    }
  }
  var resamplerType: ResamplerType = .asyncSinc {
    didSet {
      defaults.set(resamplerType.rawValue, forKey: "resamplerType")
      onChanged?()
    }
  }
  var resamplerProfile: ResamplerProfile = .balanced {
    didSet {
      defaults.set(resamplerProfile.rawValue, forKey: "resamplerProfile")
      onChanged?()
    }
  }
  var resamplerInterpolation: ResamplerInterpolation = .cubic {
    didSet {
      defaults.set(resamplerInterpolation.rawValue, forKey: "resamplerInterpolation")
      onChanged?()
    }
  }
  var volume: Double = 0.0 {
    didSet { defaults.set(volume, forKey: "volume") }
  }
  var isMuted: Bool = false {
    didSet { defaults.set(isMuted, forKey: "isMuted") }
  }
  var camillaDSPPath: String = "" {
    didSet { defaults.set(camillaDSPPath, forKey: "camillaDSPPath") }
  }

  /// Fired when a setting that affects the DSP config changes. Volume and mute are excluded
  /// because they are applied as live engine commands by DSPEngineController, not via a full
  /// config rebuild.
  var onChanged: (() -> Void)?

  func loadPreferences() {
    let savedChunkSize = defaults.integer(forKey: "chunksize")
    chunkSize = savedChunkSize > 0 ? savedChunkSize : 1024
    volume = defaults.double(forKey: "volume")
    isMuted = defaults.bool(forKey: "isMuted")
    enableRateAdjust = defaults.bool(forKey: "enableRateAdjust")
    resamplerEnabled = defaults.bool(forKey: "resamplerEnabled")
    if let t = defaults.string(forKey: "resamplerType"), let type = ResamplerType(rawValue: t) {
      resamplerType = type
    }
    if let p = defaults.string(forKey: "resamplerProfile"),
      let profile = ResamplerProfile(rawValue: p)
    {
      resamplerProfile = profile
    }
    if let i = defaults.string(forKey: "resamplerInterpolation"),
      let interpolation = ResamplerInterpolation(rawValue: i)
    {
      resamplerInterpolation = interpolation
    }
    camillaDSPPath = defaults.string(forKey: "camillaDSPPath") ?? ""
  }
}
