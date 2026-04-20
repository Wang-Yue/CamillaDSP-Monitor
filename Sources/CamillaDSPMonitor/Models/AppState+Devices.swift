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
    if let name = selectedCaptureDevice {
      let desc = await engine.getDeviceCapabilities(
        backend: "coreaudio", device: name, isCapture: true)
      captureCapabilities = desc
      let capChannels = desc?.availableChannels() ?? []
      if !capChannels.isEmpty && !capChannels.contains(captureChannels) {
        captureChannels = capChannels.contains(2) ? 2 : capChannels[0]
      }
      captureSupportedRates = desc?.sampleRates(forChannels: captureChannels) ?? []
      print("[AppState] Capture \(name): channels \(capChannels) rates \(captureSupportedRates)")
    } else {
      captureCapabilities = nil
      captureSupportedRates = []
      captureSupportedFormats = []
    }

    if let name = selectedPlaybackDevice {
      let desc = await engine.getDeviceCapabilities(
        backend: "coreaudio", device: name, isCapture: false)
      playbackCapabilities = desc
      let pbChannels = desc?.availableChannels() ?? []
      if !pbChannels.isEmpty && !pbChannels.contains(playbackChannels) {
        playbackChannels = pbChannels.contains(2) ? 2 : pbChannels[0]
      }
      playbackSupportedRates = desc?.sampleRates(forChannels: playbackChannels) ?? []
      print("[AppState] Playback \(name): channels \(pbChannels) rates \(playbackSupportedRates)")
    } else {
      playbackCapabilities = nil
      playbackSupportedRates = []
      playbackSupportedFormats = []
    }

    refreshFormatsFromCapabilities()
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
