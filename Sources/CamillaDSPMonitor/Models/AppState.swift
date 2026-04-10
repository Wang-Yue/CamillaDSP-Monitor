// AppState - Central observable state for the entire app

import CamillaDSPLib
import Combine
import CoreAudio
import SwiftUI

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

public enum AppStatus: Equatable, Sendable {
  case inactive
  case starting
  case running
  case applyingConfig
  case error(String)
}

@MainActor
final class AppState: ObservableObject {
  let defaults = UserDefaults.standard

  @Published var status: AppStatus = .inactive

  var isRunning: Bool {
    if case .running = status { return true }
    if case .applyingConfig = status { return true }
    return false
  }

  var isBusy: Bool {
    status == .starting || status == .applyingConfig
  }

  @Published var lastError: String?

  @Published var captureDevices: [AudioDevice] = []
  @Published var playbackDevices: [AudioDevice] = []

  @Published var captureSupportedRates: [Int] = []
  @Published var playbackSupportedRates: [Int] = []

  var captureRateOptions: [Int] {
    if resamplerEnabled {
      return captureSupportedRates
    } else {
      if captureSupportedRates.isEmpty { return playbackSupportedRates }
      if playbackSupportedRates.isEmpty { return captureSupportedRates }
      let common = Set(captureSupportedRates).intersection(Set(playbackSupportedRates)).sorted()
      return common.isEmpty ? playbackSupportedRates : common
    }
  }

  var playbackRateOptions: [Int] {
    if resamplerEnabled {
      return playbackSupportedRates
    } else {
      return captureRateOptions
    }
  }

  @Published var selectedCaptureDevice: String? = nil {
    didSet {
      defaults.set(selectedCaptureDevice, forKey: Keys.captureDevice)
      self.refreshSupportedRates()
      self.validateSampleRates()
      self.refreshSupportedFormats()
      if !isLoadingPreferences { startSampleRateListeners() }
      applyConfig()
    }
  }
  @Published var selectedPlaybackDevice: String? = nil {
    didSet {
      defaults.set(selectedPlaybackDevice, forKey: Keys.playbackDevice)
      self.refreshSupportedRates()
      self.validateSampleRates()
      self.refreshSupportedFormats()
      if !isLoadingPreferences { startSampleRateListeners() }
      applyConfig()
    }
  }
  @Published var captureChannels: Int = 2 {
    didSet {
      defaults.set(captureChannels, forKey: Keys.captureChannels)
      applyConfig()
    }
  }
  @Published var playbackChannels: Int = 2 {
    didSet {
      defaults.set(playbackChannels, forKey: Keys.playbackChannels)
      applyConfig()
    }
  }
  @Published var exclusiveMode: Bool = false {
    didSet {
      defaults.set(exclusiveMode, forKey: Keys.exclusiveMode)
      applyConfig()
    }
  }

  @Published var captureSampleRate: Int = 48000 {
    didSet {
      defaults.set(captureSampleRate, forKey: Keys.captureSampleRate)
      if !isLoadingPreferences {
        refreshSupportedFormats()
      }
      applyConfig()
    }
  }
  @Published var playbackSampleRate: Int = 48000 {
    didSet {
      defaults.set(playbackSampleRate, forKey: Keys.playbackSampleRate)
      if !isLoadingPreferences {
        syncCaptureRateIfNeeded()
        refreshSupportedFormats()
      }
      applyConfig()
    }
  }

  @Published var captureFormat: String = "F32"
  @Published var playbackFormat: String = "F32"

  @Published var camillaDSPPath: String = "" {
    didSet {
      defaults.set(camillaDSPPath, forKey: Keys.camillaDSPPath)
    }
  }

  var sampleRate: Int { captureSampleRate }
  var latencyMs: Double { Double(chunkSize) / Double(captureSampleRate) * 1000.0 }
  @Published var chunkSize: Int = 1024 {
    didSet {
      defaults.set(chunkSize, forKey: Keys.chunkSize)
      applyConfig()
    }
  }
  @Published var enableRateAdjust: Bool = false {
    didSet {
      defaults.set(enableRateAdjust, forKey: Keys.enableRateAdjust)
      applyConfig()
    }
  }
  @Published var resamplerEnabled: Bool = false {
    didSet {
      defaults.set(resamplerEnabled, forKey: Keys.resamplerEnabled)
      validateSampleRates()
      refreshSupportedFormats()
      applyConfig()
    }
  }
  @Published var resamplerType: ResamplerType = .asyncSinc {
    didSet {
      defaults.set(resamplerType.rawValue, forKey: Keys.resamplerType)
      applyConfig()
    }
  }
  @Published var resamplerProfile: ResamplerProfile = .balanced {
    didSet {
      defaults.set(resamplerProfile.rawValue, forKey: Keys.resamplerProfile)
      applyConfig()
    }
  }
  @Published var resamplerInterpolation: ResamplerInterpolation = .cubic {
    didSet {
      defaults.set(resamplerInterpolation.rawValue, forKey: Keys.resamplerInterpolation)
      applyConfig()
    }
  }

  @Published var volume: Double = 0.0 {
    didSet { defaults.set(volume, forKey: Keys.volume) }
  }
  @Published var isMuted: Bool = false {
    didSet { defaults.set(isMuted, forKey: Keys.isMuted) }
  }

  @Published var stages: [PipelineStage] = PipelineStage.defaultStages()
  @Published var eqPresets: [EQPreset] = []
  let levels = LevelState()
  let spectrum = SpectrumState()
  let load = LoadState()

  let engine = DSPEngine()
  var isLoadingPreferences = false
  var spectrumAnalyzer: FFTSpectrumAnalyzer? {
    didSet { analyzerRef.analyzer = spectrumAnalyzer }
  }
  /// Thread-safe reference to the current spectrum analyzer, used by the audio tap callback
  /// to avoid accessing @MainActor-isolated properties from the audio render thread.
  let analyzerRef = AnalyzerRef()
  var audioTap: CoreAudioTap?
  var lastAppliedConfigYAML: String?
  var isPollingLevels = false
  var startEngineTask: Task<Void, Never>?
  var spectrumRestartTask: Task<Void, Never>?
  var monitoringTask: Task<Void, Never>?
  var vuSubscriptionTask: Task<Void, Never>?
  var stateSubscriptionTask: Task<Void, Never>?
  var isVuSubscriptionActive = false
  var isStateSubscriptionActive = false
  var monitoredCaptureDeviceID: AudioDeviceID?
  var monitoredPlaybackDeviceID: AudioDeviceID?
  var captureRateListenerBlock: AudioObjectPropertyListenerBlock?
  var playbackRateListenerBlock: AudioObjectPropertyListenerBlock?

  // Recovery Throttling
  var lastRecoveryTime: Date?
  var pollCounter: Int = 0

  enum Keys {
    static let captureDevice = "captureDevice"
    static let playbackDevice = "playbackDevice"
    static let captureChannels = "captureChannels"
    static let playbackChannels = "playbackChannels"
    static let captureSampleRate = "captureSampleRate"
    static let playbackSampleRate = "playbackSampleRate"
    static let chunkSize = "chunksize"
    static let volume = "volume"
    static let isMuted = "isMuted"
    static let enableRateAdjust = "enableRateAdjust"
    static let exclusiveMode = "exclusiveMode"
    static let resamplerEnabled = "resamplerEnabled"
    static let resamplerType = "resamplerType"
    static let resamplerProfile = "resamplerProfile"
    static let resamplerInterpolation = "resamplerInterpolation"
    static let camillaDSPPath = "camillaDSPPath"
  }

  init() {
    print("[AppState] Initializing...")
    DSPEngine.killStaleCamillaDSP()

    isLoadingPreferences = true
    loadPreferences()
    eqPresets = loadEQPresets()
    createDefaultEQPresetsIfNeeded()
    loadPipelineStages()
    isLoadingPreferences = false

    startDeviceChangeListener()

    let ref = analyzerRef
    audioTap = CoreAudioTap(onAudio: { waveform in
      ref.analyzer?.enqueueAudio(waveform)
    })

    Task {
      do {
        if camillaDSPPath.isEmpty {
          let home = ProcessInfo.processInfo.environment["HOME"] ?? ""
          let defaultPaths = [
            "\(home)/camilladsp/target/release/camilladsp",
            "\(home)/Downloads/camilladsp",
            "/usr/local/bin/camilladsp",
            "/opt/homebrew/bin/camilladsp",
          ]
          for path in defaultPaths {
            if FileManager.default.fileExists(atPath: path) {
              camillaDSPPath = path
              break
            }
          }
        }

        try await engine.connect(binaryPath: camillaDSPPath)
        await fetchDevices()
        self.refreshSupportedRates()
        self.validateSampleRates()
        self.refreshSupportedFormats()
      } catch {
        print("[AppState] Initial connection failed: \(error)")
      }
    }
  }

  func validateSampleRates() {
    guard !isLoadingPreferences else { return }
    let pbOptions = playbackRateOptions
    if !pbOptions.isEmpty && !pbOptions.contains(playbackSampleRate) {
      playbackSampleRate = Self.bestRate(from: pbOptions, preferring: playbackSampleRate)
    }
    let capOptions = captureRateOptions
    if !capOptions.isEmpty && !capOptions.contains(captureSampleRate) {
      captureSampleRate = Self.bestRate(from: capOptions, preferring: captureSampleRate)
    }
    if !resamplerEnabled && captureSampleRate != playbackSampleRate {
      captureSampleRate = playbackSampleRate
    }
  }

  private static func bestRate(from rates: [Int], preferring current: Int) -> Int {
    if rates.contains(current) { return current }
    // Prefer common audiophile rates, then nearest
    for preferred in [48000, 44100, 96000, 192000] {
      if rates.contains(preferred) { return preferred }
    }
    return rates.min(by: { abs($0 - current) < abs($1 - current) }) ?? 48000
  }

  private func syncCaptureRateIfNeeded() {
    guard !resamplerEnabled && !isLoadingPreferences else { return }
    if captureSampleRate != playbackSampleRate {
      captureSampleRate = playbackSampleRate
    }
  }

  private func loadPreferences() {
    selectedCaptureDevice = defaults.string(forKey: Keys.captureDevice)
    selectedPlaybackDevice = defaults.string(forKey: Keys.playbackDevice)
    let savedCaptureChannels = defaults.integer(forKey: Keys.captureChannels)
    captureChannels = savedCaptureChannels > 0 ? savedCaptureChannels : 2
    let savedPlaybackChannels = defaults.integer(forKey: Keys.playbackChannels)
    playbackChannels = savedPlaybackChannels > 0 ? savedPlaybackChannels : 2
    let savedCapRate = defaults.integer(forKey: Keys.captureSampleRate)
    if savedCapRate > 0 { captureSampleRate = savedCapRate }
    let savedPbRate = defaults.integer(forKey: Keys.playbackSampleRate)
    if savedPbRate > 0 { playbackSampleRate = savedPbRate }
    let savedChunkSize = defaults.integer(forKey: Keys.chunkSize)
    chunkSize = savedChunkSize > 0 ? savedChunkSize : 1024
    volume = defaults.double(forKey: Keys.volume)
    isMuted = defaults.bool(forKey: Keys.isMuted)
    enableRateAdjust = defaults.bool(forKey: Keys.enableRateAdjust)
    exclusiveMode = defaults.bool(forKey: Keys.exclusiveMode)
    resamplerEnabled = defaults.bool(forKey: Keys.resamplerEnabled)
    if let t = defaults.string(forKey: Keys.resamplerType), let type = ResamplerType(rawValue: t) {
      resamplerType = type
    }
    if let p = defaults.string(forKey: Keys.resamplerProfile),
      let profile = ResamplerProfile(rawValue: p)
    {
      resamplerProfile = profile
    }
    if let i = defaults.string(forKey: Keys.resamplerInterpolation),
      let interpolation = ResamplerInterpolation(rawValue: i)
    {
      resamplerInterpolation = interpolation
    }
    camillaDSPPath = defaults.string(forKey: Keys.camillaDSPPath) ?? ""
  }
}
