// AudioSettings - Processing parameters and user preferences

import Foundation
import Observation

enum ResamplerType: String, Codable, Sendable, CaseIterable, Identifiable {
  case synchronous = "Synchronous"
  var id: String { rawValue }
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
  var resamplerType: ResamplerType = .synchronous {
    didSet {
      defaults.set(resamplerType.rawValue, forKey: "resamplerType")
      onChanged?()
    }
  }
  var volume: Float = 0.0 {
    didSet { defaults.set(volume, forKey: "volume") }
  }
  var isMuted: Bool = false {
    didSet { defaults.set(isMuted, forKey: "isMuted") }
  }
  var silenceThreshold: Int = -60 {
    didSet {
      defaults.set(silenceThreshold, forKey: "silenceThreshold")
      onChanged?()
    }
  }
  var silenceTimeout: Int = 0 {
    didSet {
      defaults.set(silenceTimeout, forKey: "silenceTimeout")
      onChanged?()
    }
  }

  /// Fired when a setting that affects the DSP config changes. Volume and mute are excluded
  /// because they are applied as live engine commands by DSPEngineController, not via a full
  /// config rebuild.
  var onChanged: (() -> Void)?

  func loadPreferences() {
    let savedChunkSize = defaults.integer(forKey: "chunksize")
    chunkSize = savedChunkSize > 0 ? savedChunkSize : 1024
    volume = defaults.float(forKey: "volume")
    isMuted = defaults.bool(forKey: "isMuted")
    enableRateAdjust = defaults.bool(forKey: "enableRateAdjust")
    resamplerEnabled = defaults.bool(forKey: "resamplerEnabled")

    silenceThreshold = defaults.object(forKey: "silenceThreshold") as? Int ?? -60
    silenceTimeout = defaults.object(forKey: "silenceTimeout") as? Int ?? 0

    if let t = defaults.string(forKey: "resamplerType"), let type = ResamplerType(rawValue: t) {
      resamplerType = type
    }
  }
}
