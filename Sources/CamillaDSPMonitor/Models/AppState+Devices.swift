// AppState+Devices - Audio device management using the CamillaDSP capabilities WebSocket API

import CamillaDSPLib
import CoreAudio
import Foundation

/// Sendable wrapper for a weak reference to a MainActor-isolated object.
final class WeakRef<T: AnyObject>: @unchecked Sendable {
  weak var value: T?
  init(_ value: T) { self.value = value }
}

extension AppState {

  func fetchDevices() async {
    let cap = await engine.getAvailableDevices(backend: "coreaudio", input: true)
    let pb = await engine.getAvailableDevices(backend: "coreaudio", input: false)
    self.captureDevices = cap
    self.playbackDevices = pb
    await refreshDeviceCapabilities()
  }

  func refreshDevices() {
    Task { await fetchDevices() }
  }

  // MARK: - Device Capabilities

  /// Fetches capabilities for the currently selected devices from CamillaDSP, then
  /// updates supported sample rates and available formats for both capture and playback.
  func refreshDeviceCapabilities() async {
    // Phase 1: Fetch capabilities and compute derived values into locals.
    // No state is written here, so no didSet chains fire against partial data.
    let newCapDesc: AudioDeviceDescriptor?
    let newCapChannels: Int
    if let name = selectedCaptureDevice {
      let desc = await engine.getDeviceCapabilities(
        backend: "coreaudio", device: name, isCapture: true)
      let supported = desc?.availableChannels() ?? []
      newCapDesc = desc
      newCapChannels = snappedChannels(current: captureChannels, supported: supported)
      print("[AppState] Capture \(name): channels \(supported)")
    } else {
      newCapDesc = nil
      newCapChannels = captureChannels
    }

    let newPbDesc: AudioDeviceDescriptor?
    let newPbChannels: Int
    if let name = selectedPlaybackDevice {
      let desc = await engine.getDeviceCapabilities(
        backend: "coreaudio", device: name, isCapture: false)
      let supported = desc?.availableChannels() ?? []
      newPbDesc = desc
      newPbChannels = snappedChannels(current: playbackChannels, supported: supported)
      print("[AppState] Playback \(name): channels \(supported)")
    } else {
      newPbDesc = nil
      newPbChannels = playbackChannels
    }

    // Phase 2: Write all state in one suppressed batch so intermediate didSet
    // chains (validateSampleRates, applyConfig) don't fire against half-written state.
    // Capabilities are plain vars (no didSet), so they're set first so that the
    // channels didSet's call to refreshRatesFromCapabilities reads correct data.
    isLoadingPreferences = true
    captureCapabilities = newCapDesc
    playbackCapabilities = newPbDesc
    captureChannels = newCapChannels
    playbackChannels = newPbChannels
    isLoadingPreferences = false

    // Phase 3: Single cascade from a fully consistent state.
    refreshRatesFromCapabilities()
    refreshFormatsFromCapabilities()
  }

  /// Returns `current` if it is in `supported`, otherwise snaps to 2 (preferred)
  /// or the first available channel count.
  private func snappedChannels(current: Int, supported: [Int]) -> Int {
    guard !supported.isEmpty else { return current }
    if supported.contains(current) { return current }
    return supported.contains(2) ? 2 : supported[0]
  }

  /// Re-derives supported rates from cached capabilities when channel count changes.
  /// Synchronous — no network call needed.
  func refreshRatesFromCapabilities() {
    captureSupportedRates = captureCapabilities?.sampleRates(forChannels: captureChannels) ?? []
    playbackSupportedRates =
      playbackCapabilities?.sampleRates(forChannels: playbackChannels) ?? []
  }

  /// Re-derives the available format lists from cached capabilities when sample rate or
  /// channel count changes. Only resets the user's format choice if it is no longer supported.
  /// Synchronous — no network call needed.
  func refreshFormatsFromCapabilities() {
    let capFormats =
      captureCapabilities?.availableFormats(
        channels: captureChannels, sampleRate: captureSampleRate) ?? []
    captureSupportedFormats = capFormats
    if !capFormats.isEmpty && !capFormats.contains(captureFormat) {
      captureFormat = capFormats.first ?? "F32"
    }

    let pbFormats =
      playbackCapabilities?.availableFormats(
        channels: playbackChannels, sampleRate: playbackSampleRate) ?? []
    playbackSupportedFormats = pbFormats
    if !pbFormats.isEmpty && !pbFormats.contains(playbackFormat) {
      playbackFormat = pbFormats.first ?? "F32"
    }
  }

  // MARK: - System Device Change Listener

  func startDeviceChangeListener() {
    let weakSelf = WeakRef(self)
    Self.addDeviceChangeListener(weakSelf: weakSelf)
  }

  private nonisolated static func addDeviceChangeListener(weakSelf: WeakRef<AppState>) {
    var address = AudioObjectPropertyAddress(
      mSelector: kAudioHardwarePropertyDevices,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain)

    AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, nil) {
      _, _ in
      Task { @MainActor in
        print("[AppState] Audio devices changed, refreshing list")
        weakSelf.value?.refreshDevices()
      }
    }
  }

  // MARK: - Helpers

  func devicesAvailable() -> Bool {
    if let name = selectedCaptureDevice {
      if !captureDevices.contains(where: { $0.name == name }) { return false }
    }
    if let name = selectedPlaybackDevice {
      if !playbackDevices.contains(where: { $0.name == name }) { return false }
    }
    return true
  }
}
