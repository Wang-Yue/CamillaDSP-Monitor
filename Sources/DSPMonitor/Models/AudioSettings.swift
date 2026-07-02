// AudioSettings - Processing parameters and user preferences

import Foundation
import Observation

// The Monitor's UI exposes every resampler type the *Rust* engine can
// run, even though the Swift-native lib currently implements only the
// `.synchronous` and `.apple` types — `.asyncSinc` and `.asyncPoly`
// are valid choices when running `make ENGINE=rust`. The Swift-engine
// fallback (mapping unsupported choices onto `.synchronous`) lives in
// `DSPEngineController.applyConfig`.
enum ResamplerType: String, Codable, Sendable, CaseIterable, Identifiable {
  case asyncSinc = "AsyncSinc"
  case asyncPoly = "AsyncPoly"
  case synchronous = "Synchronous"
  case apple = "Apple"
  var id: String { rawValue }
}

enum ResamplerProfile: String, Codable, Sendable, CaseIterable, Identifiable {
  case veryFast = "VeryFast"
  case fast = "Fast"
  case balanced = "Balanced"
  case accurate = "Accurate"
  var id: String { rawValue }
}

enum ResamplerInterpolation: String, Codable, Sendable, CaseIterable, Identifiable {
  case linear = "Linear"
  case cubic = "Cubic"
  case quintic = "Quintic"
  case septic = "Septic"
  var id: String { rawValue }
}

enum ResamplerAppleQuality: String, Codable, Sendable, CaseIterable, Identifiable {
  case min = "Min"
  case low = "Low"
  case medium = "Medium"
  case high = "High"
  case max = "Max"
  var id: String { rawValue }
}

enum ResamplerAppleComplexity: String, Codable, Sendable, CaseIterable, Identifiable {
  case linear = "Linear"
  case normal = "Normal"
  case mastering = "Mastering"
  case minimumPhase = "MinimumPhase"
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
  var resamplerProfile: ResamplerProfile = .balanced {
    didSet {
      defaults.set(resamplerProfile.rawValue, forKey: "resamplerProfile")
      onChanged?()
    }
  }
  var resamplerUseProfile: Bool = true {
    didSet {
      defaults.set(resamplerUseProfile, forKey: "resamplerUseProfile")
      onChanged?()
    }
  }
  var resamplerSincLen: Int = 256 {
    didSet {
      defaults.set(resamplerSincLen, forKey: "resamplerSincLen")
      onChanged?()
    }
  }
  var resamplerOversamplingFactor: Int = 128 {
    didSet {
      defaults.set(resamplerOversamplingFactor, forKey: "resamplerOversamplingFactor")
      onChanged?()
    }
  }
  var resamplerWindow: String = "BlackmanHarris" {
    didSet {
      defaults.set(resamplerWindow, forKey: "resamplerWindow")
      onChanged?()
    }
  }
  var resamplerFCutoff: Double = 0.95 {
    didSet {
      defaults.set(resamplerFCutoff, forKey: "resamplerFCutoff")
      onChanged?()
    }
  }
  var resamplerInterpolation: ResamplerInterpolation = .cubic {
    didSet {
      defaults.set(resamplerInterpolation.rawValue, forKey: "resamplerInterpolation")
      onChanged?()
    }
  }
  var resamplerAppleQuality: ResamplerAppleQuality = .max {
    didSet {
      defaults.set(resamplerAppleQuality.rawValue, forKey: "resamplerAppleQuality")
      onChanged?()
    }
  }
  var resamplerAppleComplexity: ResamplerAppleComplexity = .normal {
    didSet {
      defaults.set(resamplerAppleComplexity.rawValue, forKey: "resamplerAppleComplexity")
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
  var queuelimit: Int = 4 {
    didSet {
      defaults.set(queuelimit, forKey: "queuelimit")
      onChanged?()
    }
  }
  var stopOnRateChange: Bool = false {
    didSet {
      defaults.set(stopOnRateChange, forKey: "stopOnRateChange")
      onChanged?()
    }
  }
  var rateMeasureInterval: Double = 1.0 {
    didSet {
      defaults.set(rateMeasureInterval, forKey: "rateMeasureInterval")
      onChanged?()
    }
  }
  var multithreaded: Bool = false {
    didSet {
      defaults.set(multithreaded, forKey: "multithreaded")
      onChanged?()
    }
  }
  var workerThreads: Int = 0 {
    didSet {
      defaults.set(workerThreads, forKey: "workerThreads")
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
    queuelimit = defaults.object(forKey: "queuelimit") as? Int ?? 4
    stopOnRateChange = defaults.bool(forKey: "stopOnRateChange")
    rateMeasureInterval = defaults.object(forKey: "rateMeasureInterval") as? Double ?? 1.0
    multithreaded = defaults.bool(forKey: "multithreaded")
    workerThreads = defaults.object(forKey: "workerThreads") as? Int ?? 0

    if let t = defaults.string(forKey: "resamplerType"), let type = ResamplerType(rawValue: t) {
      resamplerType = type
    }
    if let p = defaults.string(forKey: "resamplerProfile"),
      let profile = ResamplerProfile(rawValue: p)
    {
      resamplerProfile = profile
    }
    resamplerUseProfile = defaults.object(forKey: "resamplerUseProfile") as? Bool ?? true
    resamplerSincLen = defaults.integer(forKey: "resamplerSincLen")
    if resamplerSincLen == 0 { resamplerSincLen = 256 }
    resamplerOversamplingFactor = defaults.integer(forKey: "resamplerOversamplingFactor")
    if resamplerOversamplingFactor == 0 { resamplerOversamplingFactor = 128 }
    resamplerWindow = defaults.string(forKey: "resamplerWindow") ?? "BlackmanHarris"
    resamplerFCutoff = defaults.object(forKey: "resamplerFCutoff") as? Double ?? 0.95
    if let i = defaults.string(forKey: "resamplerInterpolation"),
      let interpolation = ResamplerInterpolation(rawValue: i)
    {
      resamplerInterpolation = interpolation
    }
    if let q = defaults.string(forKey: "resamplerAppleQuality"),
      let quality = ResamplerAppleQuality(rawValue: q)
    {
      resamplerAppleQuality = quality
    }
    if let c = defaults.string(forKey: "resamplerAppleComplexity"),
      let complexity = ResamplerAppleComplexity(rawValue: c)
    {
      resamplerAppleComplexity = complexity
    }
  }
}
