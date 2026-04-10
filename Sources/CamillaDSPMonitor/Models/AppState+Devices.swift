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
          let label = isCapture ? "capture" : "playback"
          let currentRate = isCapture ? self.captureSampleRate : self.playbackSampleRate
          guard newRate != currentRate else { return }

          let supportedRates = isCapture ? self.captureSupportedRates : self.playbackSupportedRates
          guard supportedRates.isEmpty || supportedRates.contains(newRate) else {
            print("[AppState] Ignoring unsupported \(label) rate \(newRate) Hz")
            return
          }

          print("[AppState] Hardware \(label) sample rate changed to \(newRate) Hz")
          if isCapture {
            self.captureSampleRate = newRate
          } else {
            self.playbackSampleRate = newRate
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

}

extension AppState {
  func refreshSupportedRates() {
    if let captureName = selectedCaptureDevice, let id = findCoreAudioDeviceID(name: captureName) {
      captureSupportedRates = getSupportedSampleRates(deviceID: id, isCapture: true)
      print("[AppState] Supported capture rates for \(captureName): \(captureSupportedRates)")
    } else {
      captureSupportedRates = []
    }

    if let playbackName = selectedPlaybackDevice, let id = findCoreAudioDeviceID(name: playbackName)
    {
      playbackSupportedRates = getSupportedSampleRates(deviceID: id, isCapture: false)
      print("[AppState] Supported playback rates for \(playbackName): \(playbackSupportedRates)")
    } else {
      playbackSupportedRates = []
    }
  }

  func refreshSupportedFormats() {
    if let captureName = selectedCaptureDevice, let id = findCoreAudioDeviceID(name: captureName) {
      captureFormat = getBestFormat(deviceID: id, isCapture: true, atRate: captureSampleRate)
      print("[AppState] Best capture format at \(captureSampleRate) Hz for \(captureName): \(captureFormat)")
    }

    if let playbackName = selectedPlaybackDevice, let id = findCoreAudioDeviceID(name: playbackName)
    {
      playbackFormat = getBestFormat(deviceID: id, isCapture: false, atRate: playbackSampleRate)
      print("[AppState] Best playback format at \(playbackSampleRate) Hz for \(playbackName): \(playbackFormat)")
    }
  }

  private func getSupportedSampleRates(deviceID: AudioDeviceID, isCapture: Bool) -> [Int] {
    let scope =
      isCapture ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput

    // First try per-direction scope for devices that report different rates per direction
    var rates = querySampleRates(deviceID: deviceID, scope: scope)

    // Fallback to global scope if per-direction returns nothing (some devices only report globally)
    if rates.isEmpty {
      rates = querySampleRates(deviceID: deviceID, scope: kAudioObjectPropertyScopeGlobal)
    }

    return rates
  }

  private func querySampleRates(deviceID: AudioDeviceID, scope: AudioObjectPropertyScope) -> [Int] {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyAvailableNominalSampleRates,
      mScope: scope,
      mElement: kAudioObjectPropertyElementMain)

    var size: UInt32 = 0
    let status = AudioObjectGetPropertyDataSize(deviceID, &addr, 0, nil, &size)
    if status != noErr { return [] }

    let count = Int(size) / MemoryLayout<AudioValueRange>.size
    var ranges = [AudioValueRange](repeating: AudioValueRange(), count: count)
    let dataStatus = AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &size, &ranges)

    if dataStatus != noErr { return [] }

    var rates = Set<Int>()
    for range in ranges {
      rates.insert(Int(range.mMinimum))
      if range.mMaximum > range.mMinimum {
        rates.insert(Int(range.mMaximum))
      }
    }

    return rates.sorted()
  }

  private func getBestFormat(deviceID: AudioDeviceID, isCapture: Bool, atRate: Int) -> String {
    let scope =
      isCapture ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput

    var streamsAddr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyStreams,
      mScope: scope,
      mElement: kAudioObjectPropertyElementMain)

    var streamsSize: UInt32 = 0
    AudioObjectGetPropertyDataSize(deviceID, &streamsAddr, 0, nil, &streamsSize)
    let streamCount = Int(streamsSize) / MemoryLayout<AudioStreamID>.size
    guard streamCount > 0 else { return "F32" }

    var streams = [AudioStreamID](repeating: 0, count: streamCount)
    let status = AudioObjectGetPropertyData(
      deviceID, &streamsAddr, 0, nil, &streamsSize, &streams)
    guard status == noErr else { return "F32" }

    var formatsAddr = AudioObjectPropertyAddress(
      mSelector: kAudioStreamPropertyAvailablePhysicalFormats,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain)

    var formatsSize: UInt32 = 0
    AudioObjectGetPropertyDataSize(streams[0], &formatsAddr, 0, nil, &formatsSize)
    let formatCount = Int(formatsSize) / MemoryLayout<AudioStreamRangedDescription>.size
    guard formatCount > 0 else { return "F32" }

    var rangedDescs = [AudioStreamRangedDescription](
      repeating: AudioStreamRangedDescription(), count: formatCount)
    let fStatus = AudioObjectGetPropertyData(
      streams[0], &formatsAddr, 0, nil, &formatsSize, &rangedDescs)
    guard fStatus == noErr else { return "F32" }

    // Find the highest bit-depth format available at the requested sample rate
    let targetRate = Float64(atRate)
    var bestBits: UInt32 = 0
    var bestIsFloat = false
    var bestLabel = "F32"

    for desc in rangedDescs {
      let range = desc.mSampleRateRange
      guard targetRate >= range.mMinimum && targetRate <= range.mMaximum else { continue }

      let asbd = desc.mFormat
      guard asbd.mFormatID == kAudioFormatLinearPCM else { continue }

      let isFloat = (asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0
      let bits = asbd.mBitsPerChannel

      // Prefer higher bit depth; among equal bit depths prefer integer over float
      if bits > bestBits || (bits == bestBits && !isFloat && bestIsFloat) {
        bestBits = bits
        bestIsFloat = isFloat
        bestLabel = formatLabel(asbd: asbd)
      }
    }

    return bestLabel
  }

  private func formatLabel(asbd: AudioStreamBasicDescription) -> String {
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
