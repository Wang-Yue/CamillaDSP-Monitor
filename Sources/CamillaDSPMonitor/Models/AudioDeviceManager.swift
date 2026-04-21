// AudioDeviceManager - Audio device enumeration, capability fetching, and config management

import CamillaDSPLib
import CoreAudio
import Foundation

/// Sendable wrapper for a weak reference to a MainActor-isolated object.
final class WeakRef<T: AnyObject>: @unchecked Sendable {
  weak var value: T?
  init(_ value: T) { self.value = value }
}

@MainActor
final class AudioDeviceManager: ObservableObject {
  let defaults = UserDefaults.standard
  let engine: DSPEngine
  let settings: AudioSettings

  /// Fired after device config changes that require a DSP config rebuild.
  var onConfigChanged: (() -> Void)?

  @Published var captureDevices: [AudioDevice] = []
  @Published var playbackDevices: [AudioDevice] = []

  // Suppresses side-effects (capability refresh, persistence, callbacks) while init()
  // is loading persisted values. Without this guard, @Published didSet fires for every
  // assignment inside init(), spawning refreshDeviceCapabilities() Tasks before the engine
  // is connected and printing spurious "notConnected" errors on every launch.
  private var isInitializing = true

  @Published var captureConfig: DeviceConfig = DeviceConfig() {
    didSet {
      guard !isInitializing else { return }
      let enforced = captureConfig.enforced()
      if enforced != captureConfig {
        captureConfig = enforced
        return
      }
      if let data = try? JSONEncoder().encode(captureConfig) {
        defaults.set(data, forKey: "captureConfig")
      }
      if captureConfig.deviceName != oldValue.deviceName {
        Task { await refreshDeviceCapabilities() }
      } else {
        validateSampleRates()
        onConfigChanged?()
      }
    }
  }

  @Published var playbackConfig: DeviceConfig = DeviceConfig() {
    didSet {
      guard !isInitializing else { return }
      let enforced = playbackConfig.enforced()
      if enforced != playbackConfig {
        playbackConfig = enforced
        return
      }
      if let data = try? JSONEncoder().encode(playbackConfig) {
        defaults.set(data, forKey: "playbackConfig")
      }
      if playbackConfig.deviceName != oldValue.deviceName {
        Task { await refreshDeviceCapabilities() }
      } else {
        validateSampleRates()
        onConfigChanged?()
      }
    }
  }

  @Published var exclusiveMode: Bool = false {
    didSet {
      guard !isInitializing else { return }
      defaults.set(exclusiveMode, forKey: "exclusiveMode")
      onConfigChanged?()
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
    Double(settings.chunkSize) / Double(captureConfig.sampleRate) * 1000.0
  }

  // MARK: - Init

  init(engine: DSPEngine, settings: AudioSettings) {
    self.engine = engine
    self.settings = settings
    captureConfig = Self.loadDeviceConfig(key: "captureConfig", defaults: defaults)
    playbackConfig = Self.loadDeviceConfig(key: "playbackConfig", defaults: defaults)
    exclusiveMode = defaults.bool(forKey: "exclusiveMode")
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

    if let name = newCapture.deviceName {
      if let desc = await engine.getDeviceCapabilities(
        backend: "coreaudio", device: name, isCapture: true)
      {
        newCapture.capabilities = desc
      }
      print("[AudioDeviceManager] Capture \(name): channels \(newCapture.supportedChannels)")
    }

    if let name = newPlayback.deviceName {
      if let desc = await engine.getDeviceCapabilities(
        backend: "coreaudio", device: name, isCapture: false)
      {
        newPlayback.capabilities = desc
      }
      print("[AudioDeviceManager] Playback \(name): channels \(newPlayback.supportedChannels)")
    }

    captureConfig = newCapture.enforced()
    playbackConfig = newPlayback.enforced()
  }

  // MARK: - Sample Rate Validation

  func validateSampleRates() {
    let pbOptions = playbackRateOptions
    if !pbOptions.isEmpty && !pbOptions.contains(playbackConfig.sampleRate) {
      playbackConfig.sampleRate = DeviceConfig.bestRate(
        from: pbOptions, preferring: playbackConfig.sampleRate)
    }
    let capOptions = captureRateOptions
    if !capOptions.isEmpty && !capOptions.contains(captureConfig.sampleRate) {
      captureConfig.sampleRate = DeviceConfig.bestRate(
        from: capOptions, preferring: captureConfig.sampleRate)
    }
    if !settings.resamplerEnabled && captureConfig.sampleRate != playbackConfig.sampleRate {
      captureConfig.sampleRate = playbackConfig.sampleRate
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
    let weakSelf = WeakRef(self)
    Self.addDeviceChangeListener(weakSelf: weakSelf)
  }

  private nonisolated static func addDeviceChangeListener(weakSelf: WeakRef<AudioDeviceManager>) {
    var address = AudioObjectPropertyAddress(
      mSelector: kAudioHardwarePropertyDevices,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain)

    AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, nil) {
      _, _ in
      Task { @MainActor in
        print("[AudioDeviceManager] Audio devices changed, refreshing list")
        weakSelf.value?.refreshDevices()
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
