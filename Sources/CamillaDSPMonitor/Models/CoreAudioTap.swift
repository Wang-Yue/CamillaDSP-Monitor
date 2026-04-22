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

/// A Swift 6 thread-safe audio tap.
/// All hardware management and engine lifecycle logic is confined to a private Task.
final class CoreAudioTap: Sendable {
  private let processingTask: Task<Void, Never>

  init(deviceName: String?, ringBuffer: AudioRingBuffer) {
    self.processingTask = Task.detached(priority: .high) {
      let engine = AVAudioEngine()
      let inputNode = engine.inputNode

      // 1. Setup Input Device
      if let name = deviceName {
        if let deviceID = findCoreAudioDeviceID(name: name) {
          var id = deviceID
          if let au = inputNode.audioUnit {
            let status = AudioUnitSetProperty(
              au,
              kAudioOutputUnitProperty_CurrentDevice,
              kAudioUnitScope_Global,
              0,
              &id,
              UInt32(MemoryLayout<AudioDeviceID>.size))
            if status != noErr {
              print("[Tap] Failed to set input device '\(name)': \(status)")
            } else {
              print("[Tap] Set input device to '\(name)'")
            }
          }
        } else {
          print("[Tap] Could not resolve input device '\(name)'")
        }
      }

      // 2. Validate Format
      let inputFormat = inputNode.inputFormat(forBus: 0)
      guard inputFormat.sampleRate > 0, inputFormat.channelCount > 0 else {
        print("[Tap] Invalid input format (\(inputFormat.sampleRate)Hz, \(inputFormat.channelCount)ch), skipping tap")
        return
      }
      print("[Tap] Input format: \(inputFormat.sampleRate)Hz, \(inputFormat.channelCount)ch")

      // 3. Install Tap (Zero-Allocation Write)
      inputNode.installTap(onBus: 0, bufferSize: 1024, format: inputFormat) { buffer, _ in
        let frameCount = Int(buffer.frameLength)
        guard frameCount > 0, let channels = buffer.floatChannelData else { return }
        ringBuffer.writeSumming(
          left: channels[0],
          right: buffer.format.channelCount >= 2 ? channels[1] : nil,
          count: frameCount
        )
      }

      // 4. Start Engine with Retries
      engine.prepare()
      var started = false
      for attempt in 1...5 {
        if Task.isCancelled { break }
        do {
          try engine.start()
          print("[Tap] AVAudioEngine started on attempt \(attempt)")
          started = true
          break
        } catch {
          print("[Tap] AVAudioEngine start attempt \(attempt)/5 failed: \(error)")
          if attempt < 5 {
            // Shorter retry delay (250ms)
            try? await Task.sleep(nanoseconds: 250_000_000)
          }
        }
      }

      // 5. Wait for Cancellation
      if started {
        while !Task.isCancelled {
          try? await Task.sleep(nanoseconds: 100_000_000)
        }
      }

      // 6. Cleanup
      inputNode.removeTap(onBus: 0)
      if engine.isRunning {
        engine.stop()
      }
    }
  }

  deinit {
    processingTask.cancel()
  }

  func stop() async {
    processingTask.cancel()
    _ = await processingTask.result
  }
}
