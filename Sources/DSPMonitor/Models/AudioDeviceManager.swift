// AudioDeviceManager - Audio device enumeration, capability fetching, and config management

import CoreAudio
import DSPConfig
import DSPLib
import Foundation
import Observation

@MainActor
@Observable
final class AudioDeviceManager {
  let defaults = UserDefaults.standard
  let engine: DSPEngine
  let settings: AudioSettings

  /// Fired after device config changes that require a DSP config rebuild.
  var onConfigChanged: (() -> Void)?

  var captureDevices: [AudioDevice] = []
  var playbackDevices: [AudioDevice] = []

  private var captureDeviceConfigs: [String: DeviceConfig] = [:]
  private var playbackDeviceConfigs: [String: DeviceConfig] = [:]

  // Suppresses side-effects (capability refresh, persistence, callbacks) while init()
  // is loading persisted values. Without this guard, setters fire for every
  // assignment inside init(), spawning refreshDeviceCapabilities() Tasks before the engine
  // is connected and printing spurious "notConnected" errors on every launch.
  private var isInitializing = true
  private var isValidating = false
  private var capabilityRefreshTask: Task<Void, Never>?
  private var deviceChangeDebounceTask: Task<Void, Never>?

  var captureConfig: DeviceConfig = DeviceConfig() {
    didSet {
      guard !isInitializing else { return }
      var enforced = captureConfig.enforced()
      let backendChanged = (enforced.backend != oldValue.backend)
      let devChanged = (enforced.deviceName != oldValue.deviceName || backendChanged)

      if backendChanged {
        enforced.deviceName = nil
      } else if !devChanged {
        let name = enforced.deviceName ?? ""
        captureDeviceConfigs[name] = enforced
        if let data = try? JSONEncoder().encode(captureDeviceConfigs) {
          defaults.set(data, forKey: "captureDeviceConfigs")
        }
      }

      if enforced != captureConfig {
        captureConfig = enforced
        return
      }

      if let data = try? JSONEncoder().encode(captureConfig) {
        defaults.set(data, forKey: "captureConfig")
      }

      if backendChanged {
        refreshDevices()
      } else if devChanged {
        capabilityRefreshTask?.cancel()
        capabilityRefreshTask = Task { await refreshDeviceCapabilities() }
      } else {
        validateSampleRates()
        onConfigChanged?()
      }
    }
  }

  var playbackConfig: DeviceConfig = DeviceConfig() {
    didSet {
      guard !isInitializing else { return }
      var enforced = playbackConfig.enforced()
      let backendChanged = (enforced.backend != oldValue.backend)
      let devChanged = (enforced.deviceName != oldValue.deviceName || backendChanged)

      if backendChanged {
        enforced.deviceName = nil
      } else if !devChanged {
        let name = enforced.deviceName ?? ""
        playbackDeviceConfigs[name] = enforced
        if let data = try? JSONEncoder().encode(playbackDeviceConfigs) {
          defaults.set(data, forKey: "playbackDeviceConfigs")
        }
      }

      if enforced != playbackConfig {
        playbackConfig = enforced
        return
      }

      if let data = try? JSONEncoder().encode(playbackConfig) {
        defaults.set(data, forKey: "playbackConfig")
      }

      if backendChanged {
        refreshDevices()
      } else if devChanged {
        capabilityRefreshTask?.cancel()
        capabilityRefreshTask = Task { await refreshDeviceCapabilities() }
      } else {
        validateSampleRates()
        onConfigChanged?()
      }
    }
  }

  var exclusiveMode: Bool = false {
    didSet {
      guard !isInitializing else { return }
      defaults.set(exclusiveMode, forKey: "exclusiveMode")
      if playbackConfig.exclusive != exclusiveMode {
        playbackConfig.exclusive = exclusiveMode
      }
      // DoP can't run without hog mode — turning hog off would silently
      // leave a stale `outputDoP=true` that the engine then rejects at
      // open(). Clear it here so the next config push is internally
      // consistent. The chained `playbackConfig.didSet` fires
      // onConfigChanged exactly once on this path.
      if !exclusiveMode && playbackConfig.outputDoP {
        playbackConfig.outputDoP = false
      } else {
        onConfigChanged?()
      }
    }
  }

  // MARK: - Cross-device rate options

  var captureRateOptions: [Int] {
    if settings.resamplerEnabled { return captureConfig.supportedRates }
    let cap = captureConfig.supportedRates
    let pb = playbackConfig.supportedRates
    if cap.isEmpty { return pb }
    if pb.isEmpty { return cap }
    let common = Set(cap).intersection(Set(pb)).sorted()
    return common.isEmpty ? pb : common
  }

  var playbackRateOptions: [Int] {
    settings.resamplerEnabled ? playbackConfig.supportedRates : captureRateOptions
  }

  var latencyMs: Double {
    let rate = max(1, captureConfig.sampleRate)
    return Double(settings.chunkSize) / Double(rate) * 1000.0
  }

  // MARK: - Init

  init(engine: DSPEngine, settings: AudioSettings) {
    self.engine = engine
    self.settings = settings
    exclusiveMode = defaults.bool(forKey: "exclusiveMode")
    captureConfig = Self.loadDeviceConfig(key: "captureConfig", defaults: defaults)
    playbackConfig = Self.loadDeviceConfig(key: "playbackConfig", defaults: defaults)
    playbackConfig.exclusive = exclusiveMode

    if let data = defaults.data(forKey: "captureDeviceConfigs"),
       let dict = try? JSONDecoder().decode([String: DeviceConfig].self, from: data) {
      self.captureDeviceConfigs = dict
    } else {
      self.captureDeviceConfigs = [captureConfig.deviceName ?? "": captureConfig]
    }

    if let data = defaults.data(forKey: "playbackDeviceConfigs"),
       let dict = try? JSONDecoder().decode([String: DeviceConfig].self, from: data) {
      self.playbackDeviceConfigs = dict
    } else {
      self.playbackDeviceConfigs = [playbackConfig.deviceName ?? "": playbackConfig]
    }

    isInitializing = false
    startDeviceChangeListener()
  }

  // MARK: - Device Fetching

  func fetchDevices() async {
    let cap = await engine.getAvailableDevices(backend: "coreaudio", input: true)
    let pb = await engine.getAvailableDevices(backend: "coreaudio", input: false)
    captureDevices = cap
    playbackDevices = pb
    await refreshDeviceCapabilities()
  }

  func refreshDevices() {
    Task { await fetchDevices() }
  }

  // MARK: - Capabilities

  /// Fetches capabilities for the selected devices, then atomically updates both configs.
  /// Each assignment triggers one `didSet` which enforces cascade constraints and fires
  /// `onConfigChanged()` exactly once.
  func refreshDeviceCapabilities() async {
    var newCapture = captureConfig
    var newPlayback = playbackConfig

    if newCapture.backend == .coreAudio {
      let name = newCapture.deviceName ?? ""
      let origName = newCapture.capabilities.name
      if let desc = await engine.getDeviceCapabilities(
        backend: "coreaudio", device: name, isCapture: true),
        !desc.capability_sets.isEmpty
      {
        newCapture.capabilities = AudioDeviceDescriptor(
          name: origName.isEmpty ? desc.name : origName, capability_sets: desc.capability_sets)
      } else if let saved = captureDeviceConfigs[name], !saved.capabilities.capability_sets.isEmpty {
        newCapture.capabilities = saved.capabilities
      }
      if !name.isEmpty && !newCapture.capabilities.capability_sets.isEmpty {
        captureDeviceConfigs[name] = newCapture
      }
    } else {
      newCapture.capabilities = AudioDeviceDescriptor()
    }

    if newPlayback.backend == .coreAudio {
      let name = newPlayback.deviceName ?? ""
      let origName = newPlayback.capabilities.name
      if let desc = await engine.getDeviceCapabilities(
        backend: "coreaudio", device: name, isCapture: false),
        !desc.capability_sets.isEmpty
      {
        newPlayback.capabilities = AudioDeviceDescriptor(
          name: origName.isEmpty ? desc.name : origName, capability_sets: desc.capability_sets)
      } else if let saved = playbackDeviceConfigs[name], !saved.capabilities.capability_sets.isEmpty {
        newPlayback.capabilities = saved.capabilities
      }
      if !name.isEmpty && !newPlayback.capabilities.capability_sets.isEmpty {
        playbackDeviceConfigs[name] = newPlayback
      }
    } else {
      newPlayback.capabilities = AudioDeviceDescriptor()
    }

    let enforcedCapture = newCapture.enforced()
    AppLogger.info(
      "AudioDeviceManager",
      "enforcedCapture channels: \(enforcedCapture.channels) (from supported: \(enforcedCapture.supportedChannels))"
    )
    captureConfig = enforcedCapture
    playbackConfig = newPlayback.enforced()
  }

  // MARK: - Sample Rate Validation

  func validateSampleRates() {
    guard !isValidating else { return }
    isValidating = true
    defer { isValidating = false }

    let pbOptions = playbackRateOptions
    if !pbOptions.isEmpty && !pbOptions.contains(playbackConfig.sampleRate) {
      let best = DeviceConfig.bestRate(from: pbOptions, preferring: playbackConfig.sampleRate)
      if playbackConfig.sampleRate != best {
        playbackConfig.sampleRate = best
      }
    }
    let capOptions = captureRateOptions
    if !capOptions.isEmpty && !capOptions.contains(captureConfig.sampleRate) {
      let best = DeviceConfig.bestRate(from: capOptions, preferring: captureConfig.sampleRate)
      if captureConfig.sampleRate != best {
        captureConfig.sampleRate = best
      }
    }
    if !settings.resamplerEnabled && captureConfig.sampleRate != playbackConfig.sampleRate {
      captureConfig.sampleRate = playbackConfig.sampleRate
    }
    if settings.resamplerEnabled && settings.resamplerType == .slip && captureConfig.sampleRate != playbackConfig.sampleRate {
      settings.resamplerType = .synchronous
    }
  }

  // MARK: - Helpers

  func devicesAvailable() -> Bool {
    if let name = captureConfig.deviceName {
      if !captureDevices.contains(where: { $0.name == name }) { return false }
    }
    if let name = playbackConfig.deviceName {
      if !playbackDevices.contains(where: { $0.name == name }) { return false }
    }
    return true
  }

  // MARK: - System Device Change Listener

  func startDeviceChangeListener() {
    var address = AudioObjectPropertyAddress(
      mSelector: kAudioHardwarePropertyDevices,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain)

    AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, .main) {
      [weak self] _, _ in
      guard let self else { return }
      self.deviceChangeDebounceTask?.cancel()
      self.deviceChangeDebounceTask = Task { @MainActor in
        try? await Task.sleep(for: .milliseconds(500))
        guard !Task.isCancelled else { return }
        AppLogger.info("AudioDeviceManager", "Audio devices changed, refreshing list")
        self.refreshDevices()
      }
    }
  }

  private static func loadDeviceConfig(key: String, defaults: UserDefaults) -> DeviceConfig {
    if let data = defaults.data(forKey: key),
      let saved = try? JSONDecoder().decode(DeviceConfig.self, from: data)
    {
      return saved
    }
    return DeviceConfig()
  }
}
