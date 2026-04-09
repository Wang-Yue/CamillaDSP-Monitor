import AVFoundation
import Accelerate
import CamillaDSPLib
import CoreAudio
import Foundation

func findCoreAudioDeviceID(name: String) -> AudioDeviceID? {
  var address = AudioObjectPropertyAddress(
    mSelector: kAudioHardwarePropertyDevices,
    mScope: kAudioObjectPropertyScopeGlobal,
    mElement: kAudioObjectPropertyElementMain)
  var size: UInt32 = 0
  AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size)
  let count = Int(size) / MemoryLayout<AudioDeviceID>.size
  var deviceIDs = [AudioDeviceID](repeating: 0, count: count)
  AudioObjectGetPropertyData(
    AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &deviceIDs)

  for id in deviceIDs {
    var nameAddress = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyDeviceNameCFString,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain)
    var cfName: Unmanaged<CFString>?
    var nameSize = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    let status = AudioObjectGetPropertyData(id, &nameAddress, 0, nil, &nameSize, &cfName)
    if status == noErr, let deviceName = cfName?.takeRetainedValue() as String?, deviceName == name
    {
      return id
    }
  }
  return nil
}

final class CoreAudioTap {
  private let engine = AVAudioEngine()
  private let onAudio: ([Float]) -> Void

  init(onAudio: @escaping ([Float]) -> Void) {
    self.onAudio = onAudio
  }

  func start(deviceName: String?) {
    stop()

    let inputNode = engine.inputNode

    if let name = deviceName, let deviceID = findCoreAudioDeviceID(name: name) {
      var id = deviceID
      let status = AudioUnitSetProperty(
        inputNode.audioUnit!,
        kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global,
        0,
        &id,
        UInt32(MemoryLayout<AudioDeviceID>.size))
      if status != noErr {
        print("[Tap] Failed to set input device: \(status)")
      }
    }

    let inputFormat = inputNode.inputFormat(forBus: 0)
    guard inputFormat.sampleRate > 0 else {
      print("[Tap] Invalid input format, skipping tap")
      return
    }

    inputNode.installTap(onBus: 0, bufferSize: 1024, format: inputFormat) { [weak self] buffer, _ in
      guard let self = self else { return }
      let frameCount = Int(buffer.frameLength)
      guard frameCount > 0, let channels = buffer.floatChannelData else { return }

      var mono = [Float](repeating: 0, count: frameCount)
      let left = channels[0]
      if buffer.format.channelCount >= 2 {
        let right = channels[1]
        var scale: Float = 0.5
        vDSP_vadd(left, 1, right, 1, &mono, 1, vDSP_Length(frameCount))
        vDSP_vsmul(mono, 1, &scale, &mono, 1, vDSP_Length(frameCount))
      } else {
        memcpy(&mono, left, frameCount * MemoryLayout<Float>.size)
      }
      self.onAudio(mono)
    }

    do {
      try engine.start()
    } catch {
      print("[Tap] AVAudioEngine failed to start: \(error)")
      inputNode.removeTap(onBus: 0)
    }
  }

  func stop() {
    engine.inputNode.removeTap(onBus: 0)
    if engine.isRunning {
      engine.stop()
    }
  }
}
