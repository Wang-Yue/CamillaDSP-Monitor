import DSPConfig
import Foundation
import Observation

// The Monitor's UI exposes every resampler type the engine can run.
// Both the Swift-native engine and the Rust engine now fully support
// `.asyncSinc`, `.asyncPoly`, and `.synchronous` types.
// The `.apple` type is exclusive to the Swift-native engine.
enum ResamplerType: String, Codable, Sendable, CaseIterable, Identifiable {
  case asyncSinc = "AsyncSinc"
  case asyncPoly = "AsyncPoly"
  case synchronous = "Synchronous"
  case apple = "Apple"
  var id: String { rawValue }
}

enum ResamplerInterpolation: String, Codable, Sendable, CaseIterable, Identifiable {
  case linear = "Linear"
  case cubic = "Cubic"
  case quintic = "Quintic"
  case septic = "Septic"
  var id: String { rawValue }
}

enum SincInterpolation: String, Codable, Sendable, CaseIterable, Identifiable {
  case nearest = "Nearest"
  case linear = "Linear"
  case quadratic = "Quadratic"
  case cubic = "Cubic"
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
  var resamplerSincInterpolation: SincInterpolation = .cubic {
    didSet {
      defaults.set(resamplerSincInterpolation.rawValue, forKey: "resamplerSincInterpolation")
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

  var fader1Volume: Float = 0.0 {
    didSet { defaults.set(fader1Volume, forKey: "fader1Volume") }
  }
  var fader2Volume: Float = 0.0 {
    didSet { defaults.set(fader2Volume, forKey: "fader2Volume") }
  }
  var fader3Volume: Float = 0.0 {
    didSet { defaults.set(fader3Volume, forKey: "fader3Volume") }
  }
  var fader4Volume: Float = 0.0 {
    didSet { defaults.set(fader4Volume, forKey: "fader4Volume") }
  }

  var fader1Muted: Bool = false {
    didSet { defaults.set(fader1Muted, forKey: "fader1Muted") }
  }
  var fader2Muted: Bool = false {
    didSet { defaults.set(fader2Muted, forKey: "fader2Muted") }
  }
  var fader3Muted: Bool = false {
    didSet { defaults.set(fader3Muted, forKey: "fader3Muted") }
  }
  var fader4Muted: Bool = false {
    didSet { defaults.set(fader4Muted, forKey: "fader4Muted") }
  }

  func volume(for fader: Fader) -> Float {
    switch fader {
    case .main: return volume
    case .aux1: return fader1Volume
    case .aux2: return fader2Volume
    case .aux3: return fader3Volume
    case .aux4: return fader4Volume
    }
  }

  func setVolume(_ vol: Float, for fader: Fader) {
    switch fader {
    case .main: volume = vol
    case .aux1: fader1Volume = vol
    case .aux2: fader2Volume = vol
    case .aux3: fader3Volume = vol
    case .aux4: fader4Volume = vol
    }
  }

  func isMuted(for fader: Fader) -> Bool {
    switch fader {
    case .main: return isMuted
    case .aux1: return fader1Muted
    case .aux2: return fader2Muted
    case .aux3: return fader3Muted
    case .aux4: return fader4Muted
    }
  }

  func setMuted(_ muted: Bool, for fader: Fader) {
    switch fader {
    case .main: isMuted = muted
    case .aux1: fader1Muted = muted
    case .aux2: fader2Muted = muted
    case .aux3: fader3Muted = muted
    case .aux4: fader4Muted = muted
    }
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

    fader1Volume = defaults.float(forKey: "fader1Volume")
    fader2Volume = defaults.float(forKey: "fader2Volume")
    fader3Volume = defaults.float(forKey: "fader3Volume")
    fader4Volume = defaults.float(forKey: "fader4Volume")

    fader1Muted = defaults.bool(forKey: "fader1Muted")
    fader2Muted = defaults.bool(forKey: "fader2Muted")
    fader3Muted = defaults.bool(forKey: "fader3Muted")
    fader4Muted = defaults.bool(forKey: "fader4Muted")
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
    if let si = defaults.string(forKey: "resamplerSincInterpolation"),
      let interpolation = SincInterpolation(rawValue: si)
    {
      resamplerSincInterpolation = interpolation
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
