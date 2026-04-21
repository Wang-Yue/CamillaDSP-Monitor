// AudioSettings - Processing parameters and user preferences

import Foundation

@MainActor
final class AudioSettings: ObservableObject {
  let defaults = UserDefaults.standard

  @Published var chunkSize: Int = 1024 {
    didSet {
      defaults.set(chunkSize, forKey: "chunksize")
      onChanged?()
    }
  }
  @Published var enableRateAdjust: Bool = false {
    didSet {
      defaults.set(enableRateAdjust, forKey: "enableRateAdjust")
      onChanged?()
    }
  }
  @Published var resamplerEnabled: Bool = false {
    didSet {
      defaults.set(resamplerEnabled, forKey: "resamplerEnabled")
      onChanged?()
    }
  }
  @Published var resamplerType: ResamplerType = .asyncSinc {
    didSet {
      defaults.set(resamplerType.rawValue, forKey: "resamplerType")
      onChanged?()
    }
  }
  @Published var resamplerProfile: ResamplerProfile = .balanced {
    didSet {
      defaults.set(resamplerProfile.rawValue, forKey: "resamplerProfile")
      onChanged?()
    }
  }
  @Published var resamplerInterpolation: ResamplerInterpolation = .cubic {
    didSet {
      defaults.set(resamplerInterpolation.rawValue, forKey: "resamplerInterpolation")
      onChanged?()
    }
  }
  @Published var volume: Double = 0.0 {
    didSet { defaults.set(volume, forKey: "volume") }
  }
  @Published var isMuted: Bool = false {
    didSet { defaults.set(isMuted, forKey: "isMuted") }
  }
  @Published var camillaDSPPath: String = "" {
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
