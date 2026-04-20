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
      defaults.set(captureConfig.channels, forKey: Keys.captureChannels)
      defaults.set(captureConfig.sampleRate, forKey: Keys.captureSampleRate)
      defaults.set(captureConfig.format, forKey: Keys.captureFormat)
      guard !isLoadingPreferences else { return }
      validateSampleRates()
      applyConfig()
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
      defaults.set(playbackConfig.channels, forKey: Keys.playbackChannels)
      defaults.set(playbackConfig.sampleRate, forKey: Keys.playbackSampleRate)
      defaults.set(playbackConfig.format, forKey: Keys.playbackFormat)
      guard !isLoadingPreferences else { return }
      validateSampleRates()
      applyConfig()
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

  @Published var selectedCaptureDevice: String? = nil {
    didSet {
      defaults.set(selectedCaptureDevice, forKey: Keys.captureDevice)
      guard !isLoadingPreferences else { return }
      Task {
        await refreshDeviceCapabilities()
        applyConfig()
      }
    }
  }
  @Published var selectedPlaybackDevice: String? = nil {
    didSet {
      defaults.set(selectedPlaybackDevice, forKey: Keys.playbackDevice)
      guard !isLoadingPreferences else { return }
      Task {
        await refreshDeviceCapabilities()
        applyConfig()
      }
    }
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
    static let captureFormat = "captureFormat"
    static let playbackFormat = "playbackFormat"
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
    selectedCaptureDevice = defaults.string(forKey: Keys.captureDevice)
    selectedPlaybackDevice = defaults.string(forKey: Keys.playbackDevice)

    var cap = DeviceConfig()
    let savedCapChannels = defaults.integer(forKey: Keys.captureChannels)
    cap.channels = savedCapChannels > 0 ? savedCapChannels : 2
    let savedCapRate = defaults.integer(forKey: Keys.captureSampleRate)
    if savedCapRate > 0 { cap.sampleRate = savedCapRate }
    if let f = defaults.string(forKey: Keys.captureFormat) { cap.format = f }
    captureConfig = cap

    var pb = DeviceConfig()
    let savedPbChannels = defaults.integer(forKey: Keys.playbackChannels)
    pb.channels = savedPbChannels > 0 ? savedPbChannels : 2
    let savedPbRate = defaults.integer(forKey: Keys.playbackSampleRate)
    if savedPbRate > 0 { pb.sampleRate = savedPbRate }
    if let f = defaults.string(forKey: Keys.playbackFormat) { pb.format = f }
    playbackConfig = pb

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
