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

  /// Fetches capabilities for the selected devices, then atomically updates both
  /// DeviceConfig structs. Each assignment triggers one `didSet` which enforces
  /// cascade constraints and fires `applyConfig()` exactly once.
  func refreshDeviceCapabilities() async {
    var newCapture = captureConfig
    var newPlayback = playbackConfig

    if let name = newCapture.deviceName {
      if let desc = await engine.getDeviceCapabilities(
        backend: "coreaudio", device: name, isCapture: true)
      {
        newCapture.capabilities = desc
      }
      print("[AppState] Capture \(name): channels \(newCapture.supportedChannels)")
    }

    if let name = newPlayback.deviceName {
      if let desc = await engine.getDeviceCapabilities(
        backend: "coreaudio", device: name, isCapture: false)
      {
        newPlayback.capabilities = desc
      }
      print("[AppState] Playback \(name): channels \(newPlayback.supportedChannels)")
    }

    // Batch-assign both configs: one combined validateSampleRates+applyConfig fires at the end.
    withSuppressedSideEffects {
      captureConfig = newCapture.enforced()
      playbackConfig = newPlayback.enforced()
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
    if let name = captureConfig.deviceName {
      if !captureDevices.contains(where: { $0.name == name }) { return false }
    }
    if let name = playbackConfig.deviceName {
      if !playbackDevices.contains(where: { $0.name == name }) { return false }
    }
    return true
  }
}
