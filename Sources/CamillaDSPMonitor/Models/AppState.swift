// AppState - Central observable state for the entire app

import CamillaDSPLib
import Combine
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

  // MARK: - Per-device config (capabilities + selection — one notification per device)

  private var _applyingCapture = false
  private var _applyingPlayback = false

  @Published var captureConfig: DeviceConfig = DeviceConfig() {
    didSet {
      guard !_applyingCapture else { return }
      let enforced = captureConfig.enforced()
      if enforced != captureConfig {
        _applyingCapture = true
        captureConfig = enforced
        _applyingCapture = false
      }
      if let data = try? JSONEncoder().encode(captureConfig) {
        defaults.set(data, forKey: Keys.captureConfig)
      }
      guard !isLoadingPreferences else { return }
      if captureConfig.deviceName != oldValue.deviceName {
        // Device changed — fetch new capabilities; applyConfig fires via the didSet cascade
        Task { await refreshDeviceCapabilities() }
      } else {
        validateSampleRates()
        applyConfig()
      }
    }
  }

  @Published var playbackConfig: DeviceConfig = DeviceConfig() {
    didSet {
      guard !_applyingPlayback else { return }
      let enforced = playbackConfig.enforced()
      if enforced != playbackConfig {
        _applyingPlayback = true
        playbackConfig = enforced
        _applyingPlayback = false
      }
      if let data = try? JSONEncoder().encode(playbackConfig) {
        defaults.set(data, forKey: Keys.playbackConfig)
      }
      guard !isLoadingPreferences else { return }
      if playbackConfig.deviceName != oldValue.deviceName {
        Task { await refreshDeviceCapabilities() }
      } else {
        validateSampleRates()
        applyConfig()
      }
    }
  }

  // MARK: - Cross-device rate options (requires both configs — stays at AppState level)

  var captureRateOptions: [Int] {
    if resamplerEnabled { return captureConfig.supportedRates }
    let cap = captureConfig.supportedRates
    let pb = playbackConfig.supportedRates
    if cap.isEmpty { return pb }
    if pb.isEmpty { return cap }
    let common = Set(cap).intersection(Set(pb)).sorted()
    return common.isEmpty ? pb : common
  }
  var playbackRateOptions: [Int] {
    resamplerEnabled ? playbackConfig.supportedRates : captureRateOptions
  }

  @Published var exclusiveMode: Bool = false {
    didSet {
      defaults.set(exclusiveMode, forKey: Keys.exclusiveMode)
      applyConfig()
    }
  }

  @Published var camillaDSPPath: String = "" {
    didSet {
      defaults.set(camillaDSPPath, forKey: Keys.camillaDSPPath)
    }
  }

  var sampleRate: Int { captureConfig.sampleRate }
  var latencyMs: Double { Double(chunkSize) / Double(captureConfig.sampleRate) * 1000.0 }
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
  let logManager = LogManager()

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
  var startEngineTask: Task<Void, Never>?
  var spectrumRestartTask: Task<Void, Never>?
  var applyConfigTask: Task<Void, Never>?
  var monitoringTask: Task<Void, Never>?
  var vuSubscriptionTask: Task<Void, Never>?
  var stateSubscriptionTask: Task<Void, Never>?
  var isVuSubscriptionActive = false
  var isStateSubscriptionActive = false
  /// True while the mini player is the active UI — suppresses hidden main window re-renders.
  @Published var isMiniPlayerActive = false
  /// The capture device name the audio tap is currently configured for.
  var audioTapDeviceName: String?
  /// Reference count of visible spectrum views. FFT is paused when this reaches zero.
  var spectrumViewCount = 0

  // Recovery Throttling
  var lastRecoveryTime: Date?

  enum Keys {
    static let captureConfig = "captureConfig"
    static let playbackConfig = "playbackConfig"
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
    // Legacy keys — used only for one-time migration from builds before DeviceConfig
    fileprivate static let legacyCaptureDevice = "captureDevice"
    fileprivate static let legacyPlaybackDevice = "playbackDevice"
    fileprivate static let legacyCaptureChannels = "captureChannels"
    fileprivate static let legacyPlaybackChannels = "playbackChannels"
    fileprivate static let legacyCaptureSampleRate = "captureSampleRate"
    fileprivate static let legacyPlaybackSampleRate = "playbackSampleRate"
    fileprivate static let legacyCaptureFormat = "captureFormat"
    fileprivate static let legacyPlaybackFormat = "playbackFormat"
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
        self.validateSampleRates()
      } catch {
        print("[AppState] Initial connection failed: \(error)")
      }
    }
  }

  func validateSampleRates() {
    guard !isLoadingPreferences else { return }
    let pbOptions = playbackRateOptions
    if !pbOptions.isEmpty && !pbOptions.contains(playbackConfig.sampleRate) {
      playbackConfig.sampleRate = DeviceConfig.bestRate(from: pbOptions, preferring: playbackConfig.sampleRate)
    }
    let capOptions = captureRateOptions
    if !capOptions.isEmpty && !capOptions.contains(captureConfig.sampleRate) {
      captureConfig.sampleRate = DeviceConfig.bestRate(from: capOptions, preferring: captureConfig.sampleRate)
    }
    if !resamplerEnabled && captureConfig.sampleRate != playbackConfig.sampleRate {
      captureConfig.sampleRate = playbackConfig.sampleRate
    }
  }

  /// Call from a spectrum view's onAppear to keep the FFT running while visible.
  func registerSpectrumView() {
    spectrumViewCount += 1
    if spectrumViewCount == 1 { spectrumAnalyzer?.resume() }
  }

  /// Call from a spectrum view's onDisappear to allow the FFT to pause when idle.
  func unregisterSpectrumView() {
    spectrumViewCount = max(0, spectrumViewCount - 1)
    if spectrumViewCount == 0 { spectrumAnalyzer?.pause() }
  }

  private func loadPreferences() {
    captureConfig = loadDeviceConfig(key: Keys.captureConfig, legacyDeviceKey: Keys.legacyCaptureDevice,
      legacyChannelsKey: Keys.legacyCaptureChannels, legacyRateKey: Keys.legacyCaptureSampleRate,
      legacyFormatKey: Keys.legacyCaptureFormat)
    playbackConfig = loadDeviceConfig(key: Keys.playbackConfig, legacyDeviceKey: Keys.legacyPlaybackDevice,
      legacyChannelsKey: Keys.legacyPlaybackChannels, legacyRateKey: Keys.legacyPlaybackSampleRate,
      legacyFormatKey: Keys.legacyPlaybackFormat)

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

  private func loadDeviceConfig(
    key: String, legacyDeviceKey: String, legacyChannelsKey: String,
    legacyRateKey: String, legacyFormatKey: String
  ) -> DeviceConfig {
    if let data = defaults.data(forKey: key),
      let saved = try? JSONDecoder().decode(DeviceConfig.self, from: data)
    {
      return saved
    }
    // Migrate from pre-DeviceConfig separate keys
    var config = DeviceConfig(deviceName: defaults.string(forKey: legacyDeviceKey))
    let savedChannels = defaults.integer(forKey: legacyChannelsKey)
    if savedChannels > 0 { config.channels = savedChannels }
    let savedRate = defaults.integer(forKey: legacyRateKey)
    if savedRate > 0 { config.sampleRate = savedRate }
    if let f = defaults.string(forKey: legacyFormatKey) { config.format = f }
    return config
  }
}
