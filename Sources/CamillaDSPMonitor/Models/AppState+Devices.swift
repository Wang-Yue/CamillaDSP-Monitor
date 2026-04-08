// AppState+Devices - Audio device management using WebSockets and CoreAudio listeners

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

    self.startSampleRateListeners()
  }

  func refreshDevices() {
    Task { await fetchDevices() }
  }

  // MARK: - Device Listeners

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

  func startSampleRateListeners() {
    removeSampleRateListeners()
    let weakSelf = WeakRef(self)

    if let captureName = selectedCaptureDevice, let id = findCoreAudioDeviceID(name: captureName) {
      let block = Self.makeSampleRateListener(weakSelf: weakSelf, deviceID: id, isCapture: true)
      Self.addSampleRateListener(deviceID: id, block: block)
      monitoredCaptureDeviceID = id
      captureRateListenerBlock = block
    }

    if let playbackName = selectedPlaybackDevice, let id = findCoreAudioDeviceID(name: playbackName)
    {
      let block = Self.makeSampleRateListener(weakSelf: weakSelf, deviceID: id, isCapture: false)
      Self.addSampleRateListener(deviceID: id, block: block)
      monitoredPlaybackDeviceID = id
      playbackRateListenerBlock = block
    }
  }

  private func removeSampleRateListeners() {
    var address = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyNominalSampleRate,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain)

    if let id = monitoredCaptureDeviceID, let block = captureRateListenerBlock {
      AudioObjectRemovePropertyListenerBlock(id, &address, nil, block)
      monitoredCaptureDeviceID = nil
      captureRateListenerBlock = nil
    }
    if let id = monitoredPlaybackDeviceID, let block = playbackRateListenerBlock {
      AudioObjectRemovePropertyListenerBlock(id, &address, nil, block)
      monitoredPlaybackDeviceID = nil
      playbackRateListenerBlock = nil
    }
  }

  private nonisolated static func makeSampleRateListener(
    weakSelf: WeakRef<AppState>, deviceID: AudioDeviceID, isCapture: Bool
  ) -> AudioObjectPropertyListenerBlock {
    return { _, _ in
      var rate: Float64 = 0
      var size = UInt32(MemoryLayout<Float64>.size)
      var addr = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyNominalSampleRate,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain)
      let status = AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &size, &rate)

      if status == noErr {
        let newRate = Int(rate)
        Task { @MainActor in
          guard let self = weakSelf.value else { return }
          print("[AppState] Hardware sample rate changed to \(newRate) Hz")
          if isCapture {
            if self.captureSampleRate != newRate { self.captureSampleRate = newRate }
          } else {
            if self.playbackSampleRate != newRate { self.playbackSampleRate = newRate }
          }
        }
      }
    }
  }

  private nonisolated static func addSampleRateListener(
    deviceID: AudioDeviceID, block: @escaping AudioObjectPropertyListenerBlock
  ) {
    var address = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyNominalSampleRate,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain)
    AudioObjectAddPropertyListenerBlock(deviceID, &address, nil, block)
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

  func updateDetectedFormats() {
    if let captureName = selectedCaptureDevice, let id = findCoreAudioDeviceID(name: captureName) {
      captureFormat = getCoreAudioDeviceFormat(deviceID: id, isCapture: true)
      print("[AppState] Detected capture format for \(captureName): \(captureFormat)")
    }
    if let playbackName = selectedPlaybackDevice, let id = findCoreAudioDeviceID(name: playbackName)
    {
      playbackFormat = getCoreAudioDeviceFormat(deviceID: id, isCapture: false)
      print("[AppState] Detected playback format for \(playbackName): \(playbackFormat)")
    }
  }

  private func getCoreAudioDeviceFormat(deviceID: AudioDeviceID, isCapture: Bool) -> String {
    // 1. Try to get streams for the device
    var streamsAddr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyStreams,
      mScope: isCapture ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
      mElement: kAudioObjectPropertyElementMain)

    var streamsSize: UInt32 = 0
    AudioObjectGetPropertyDataSize(deviceID, &streamsAddr, 0, nil, &streamsSize)
    let streamCount = Int(streamsSize) / MemoryLayout<AudioStreamID>.size

    var asbd = AudioStreamBasicDescription()
    var asbdSize = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
    var found = false

    if streamCount > 0 {
      var streams = [AudioStreamID](repeating: 0, count: streamCount)
      let status = AudioObjectGetPropertyData(
        deviceID, &streamsAddr, 0, nil, &streamsSize, &streams)
      if status == noErr {
        // Query the Physical Format of the first stream
        var physicalAddr = AudioObjectPropertyAddress(
          mSelector: kAudioStreamPropertyPhysicalFormat,
          mScope: kAudioObjectPropertyScopeGlobal,
          mElement: kAudioObjectPropertyElementMain)

        let pStatus = AudioObjectGetPropertyData(
          streams[0], &physicalAddr, 0, nil, &asbdSize, &asbd)
        if pStatus == noErr {
          found = true
        }
      }
    }

    // 2. Fallback to device stream format if stream query failed
    if !found {
      var addr = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyStreamFormat,
        mScope: isCapture ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
        mElement: kAudioObjectPropertyElementMain)
      let status = AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &asbdSize, &asbd)
      if status != noErr { return "F32" }
    }

    let isFloat = (asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0
    let bitDepth = asbd.mBitsPerChannel

    if isFloat {
      return bitDepth == 64 ? "F64" : "F32"
    } else {
      if bitDepth == 16 { return "S16" }
      if bitDepth == 24 { return "S24" }
      if bitDepth == 32 { return "S32" }
    }

    return "F32"
  }
}

extension AppState {
  func refreshSupportedRates() {
    if let captureName = selectedCaptureDevice, let id = findCoreAudioDeviceID(name: captureName) {
      captureSupportedRates = getSupportedSampleRates(deviceID: id)
      print("[AppState] Supported capture rates for \(captureName): \(captureSupportedRates)")
    } else {
      captureSupportedRates = []
    }

    if let playbackName = selectedPlaybackDevice, let id = findCoreAudioDeviceID(name: playbackName)
    {
      playbackSupportedRates = getSupportedSampleRates(deviceID: id)
      print("[AppState] Supported playback rates for \(playbackName): \(playbackSupportedRates)")
    } else {
      playbackSupportedRates = []
    }
  }

  private func getSupportedSampleRates(deviceID: AudioDeviceID) -> [Int] {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyAvailableNominalSampleRates,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain)

    var size: UInt32 = 0
    let status = AudioObjectGetPropertyDataSize(deviceID, &addr, 0, nil, &size)
    if status != noErr { return [] }

    let count = Int(size) / MemoryLayout<AudioValueRange>.size
    var ranges = [AudioValueRange](repeating: AudioValueRange(), count: count)
    let dataStatus = AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &size, &ranges)

    if dataStatus != noErr { return [] }

    // Extract unique discrete sample rates
    var rates = Set<Int>()
    for range in ranges {
      rates.insert(Int(range.mMinimum))
      if range.mMaximum > range.mMinimum {
        rates.insert(Int(range.mMaximum))
      }
    }

    return rates.sorted()
  }
}
