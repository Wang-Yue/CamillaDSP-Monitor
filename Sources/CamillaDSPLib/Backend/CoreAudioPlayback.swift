// CamillaDSP-Swift: CoreAudio playback backend for macOS
//
// Real-time discipline
// --------------------
// The render callback runs on a high-priority audio thread driven by
// CoreAudio. It is absolutely forbidden to take locks, allocate, or
// otherwise call into the Swift runtime in a way that could block. To
// honour that:
//   - sample rings are SPSC `SPSCAudioRingBuffer<Float>` instances —
//     producer and consumer are wait-free, no `NSLock`.
//   - the AudioBufferList plus its per-channel raw data buffers are
//     preallocated in `open()` and reused for the lifetime of the unit;
//     the render callback only fills the existing struct.

import Accelerate
import AudioToolbox
import CoreAudio
import Foundation
import Logging
import Synchronization

public final class CoreAudioPlayback: PlaybackBackend {
  private let logger = Logger(label: "camilladsp.coreaudio.playback")
  private let deviceName: String?
  let channels: Int
  private let sampleRate: Double
  private let chunkSize: Int
  private let exclusive: Bool

  var audioUnit: AudioUnit?
  /// Per-channel SPSC ring buffer of `Float` samples. `write(chunk:)`
  /// is the producer; the render callback is the consumer.
  let playbackRings: [SPSCAudioRingBuffer]
  let ringBufferSize: Int

  /// HAL device the unit is bound to. Captured from the resolved
  /// device lookup in `open()` so `close()` can release hog mode
  /// without doing the lookup again (which would race a default-
  /// device change).
  private var openedDeviceID: AudioDeviceID?
  private var didAcquireHogMode = false
  /// Watches the device's nominal sample rate so the engine can
  /// surface `.playbackFormatChange` when something else flips the
  /// device rate at runtime.
  private var rateWatcher: RateChangeWatcher?

  private let _isDeviceAlive = Atomic<Bool>(true)
  private var aliveListenerBlock: AudioObjectPropertyListenerBlock?

  public var pendingRateChange: Double? { rateWatcher?.pendingRateChange }

  public var bufferLevel: Int {
    // SPSC rings carry the same available count across channels (the
    // producer fills all of them in lockstep), so channel 0 is
    // representative.
    playbackRings.first?.availableToRead ?? 0
  }

  public init(config: PlaybackDeviceConfig, sampleRate: Int, chunkSize: Int) {
    self.deviceName = config.device
    self.channels = config.channels
    self.sampleRate = Double(sampleRate)
    self.chunkSize = chunkSize
    self.exclusive = config.exclusive ?? false
    // Eight chunks of headroom matches the original lock-based design.
    self.ringBufferSize = chunkSize * 8
    self.playbackRings = (0..<config.channels).map { _ in
      SPSCAudioRingBuffer(minimumCapacity: chunkSize * 8)
    }
  }

  deinit {
    // Backstop the audio thread before tearing down — see
    // CoreAudioCapture.deinit for the rationale.
    if audioUnit != nil { close() }
  }

  public func open() throws {
    // Tear down a half-built unit if we throw partway through.
    var openSucceeded = false
    defer { if !openSucceeded { close() } }

    var desc = AudioComponentDescription(
      componentType: kAudioUnitType_Output,
      componentSubType: kAudioUnitSubType_DefaultOutput,
      componentManufacturer: kAudioUnitManufacturer_Apple,
      componentFlags: 0,
      componentFlagsMask: 0
    )

    guard let component = AudioComponentFindNext(nil, &desc) else {
      throw BackendError.deviceNotFound("No default output component found")
    }

    var unit: AudioUnit?
    var status = AudioComponentInstanceNew(component, &unit)
    guard status == noErr, let audioUnit = unit else {
      throw BackendError.initializationFailed("Failed to create output AudioUnit: \(status)")
    }
    self.audioUnit = audioUnit

    // Resolve device ID and set the named device (or use default).
    let resolvedDeviceID = CoreAudioDevice.deviceID(forName: deviceName, scope: .output)
    self.openedDeviceID = resolvedDeviceID
    if let deviceID = resolvedDeviceID, deviceName != nil {
      var id = deviceID
      status = AudioUnitSetProperty(
        audioUnit,
        kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global,
        0,
        &id,
        UInt32(MemoryLayout<AudioDeviceID>.size)
      )
      guard status == noErr else {
        throw BackendError.initializationFailed("Failed to set output device: \(status)")
      }
      if exclusive {
        var hogPID = ProcessInfo.processInfo.processIdentifier
        var hogAddress = AudioObjectPropertyAddress(
          mSelector: kAudioDevicePropertyHogMode,
          mScope: kAudioObjectPropertyScopeGlobal,
          mElement: kAudioObjectPropertyElementMain
        )
        let hogStatus = AudioObjectSetPropertyData(
          id, &hogAddress, 0, nil,
          UInt32(MemoryLayout<pid_t>.size), &hogPID
        )
        if hogStatus == noErr {
          didAcquireHogMode = true
        } else {
          logger.warning(
            "Failed to acquire hog mode (status=\(hogStatus)); continuing in shared mode")
        }
      }
    }
    // Push the device's nominal sample rate to match the engine's
    // configured rate. Without this the device may stay at its
    // previous rate and the AudioUnit silently SRCs — fine when the
    // rates align, but it leaves a latent timing mismatch when
    // capture and playback are the same physical device.
    if let deviceID = resolvedDeviceID {
      if !CoreAudioDevice.setNominalSampleRate(deviceID, sampleRate) {
        logger.warning(
          "Playback device refused \(sampleRate) Hz; AudioUnit will sample-rate convert")
      }

      let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
        guard let self = self else { return }
        var alive: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        var addr = AudioObjectPropertyAddress(
          mSelector: kAudioDevicePropertyDeviceIsAlive,
          mScope: kAudioObjectPropertyScopeGlobal,
          mElement: kAudioObjectPropertyElementMain)
        let status = AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &size, &alive)
        if status == noErr {
          self._isDeviceAlive.store(alive != 0, ordering: .releasing)
          if alive == 0 {
            self.logger.error("Playback device disconnected!")
          }
        }
      }
      self.aliveListenerBlock = block

      var address = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyDeviceIsAlive,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain)

      let status = AudioObjectAddPropertyListenerBlock(deviceID, &address, .main, block)
      if status != noErr {
        logger.warning("Failed to add alive listener: \(status)")
      }
    }

    // Set stream format: non-interleaved Float32 — same shape as the
    // capture path so chunks flow through the engine without copies.
    var streamFormat = CoreAudioDevice.float32StreamFormat(
      sampleRate: sampleRate,
      channels: channels,
      interleaved: false
    )

    status = AudioUnitSetProperty(
      audioUnit,
      kAudioUnitProperty_StreamFormat,
      kAudioUnitScope_Input,
      0,
      &streamFormat,
      UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
    )
    guard status == noErr else {
      throw BackendError.initializationFailed("Failed to set playback stream format: \(status)")
    }

    // Set render callback.
    var callbackStruct = AURenderCallbackStruct(
      inputProc: playbackCallback,
      inputProcRefCon: Unmanaged.passUnretained(self).toOpaque()
    )
    status = AudioUnitSetProperty(
      audioUnit,
      kAudioUnitProperty_SetRenderCallback,
      kAudioUnitScope_Input,
      0,
      &callbackStruct,
      UInt32(MemoryLayout<AURenderCallbackStruct>.size)
    )
    guard status == noErr else {
      throw BackendError.initializationFailed("Failed to set render callback: \(status)")
    }

    status = AudioUnitInitialize(audioUnit)
    guard status == noErr else {
      throw BackendError.initializationFailed("Failed to initialize output: \(status)")
    }

    status = AudioOutputUnitStart(audioUnit)
    guard status == noErr else {
      throw BackendError.initializationFailed("Failed to start output: \(status)")
    }

    // Register rate-change watcher after the rate has been pushed
    // — see CoreAudioCapture.open() for the rationale.
    if let deviceID = resolvedDeviceID,
      CoreAudioDevice.hasNominalSampleRateProperty(deviceID)
    {
      rateWatcher = RateChangeWatcher(deviceID: deviceID, expectedRate: sampleRate)
    }

    openSucceeded = true

    logger.info("CoreAudio playback opened: \(channels)ch @ \(sampleRate)Hz")
  }

  public func write(chunk: AudioChunk) throws {
    guard _isDeviceAlive.load(ordering: .acquiring) else {
      throw BackendError.writeError("Playback device disconnected")
    }
    let frames = chunk.validFrames
    guard frames > 0 else { return }

    let usableChannels = Swift.min(channels, chunk.channels)
    for ch in 0..<usableChannels {
      // Convert this channel's Doubles into Float and write directly
      // into the SPSC ring. No allocations, no scratch buffer needed.
      chunk.waveforms[ch].withUnsafeBufferPointer { src in
        guard let srcPtr = src.baseAddress else { return }
        playbackRings[ch].appendConvertingDoubleToFloat(srcPtr, count: frames)
      }
    }
  }

  public func prefillSilence(frames: Int) throws {
    // Push `frames` zeros into every channel ring so the
    // playback callback finds the buffer pre-filled to roughly
    // `target_level` before any real audio arrives. Without
    // this, the rate-adjust controller's first measurements
    // start from an empty buffer and have to climb to target —
    // the integral term overshoots before settling. Cap at the
    // ring's capacity to avoid blowing past the SPSC headroom.
    guard frames > 0 else { return }
    let cap = ringBufferSize
    let toWrite = Swift.min(frames, cap)
    // Write silence directly to the ring buffers without allocating memory.
    for ch in 0..<channels {
      playbackRings[ch].writeSilence(count: toWrite)
    }
    logger.info("Playback pre-filled with \(toWrite) silent frames per channel")
  }

  public func close() {
    // Drop the HAL listener *before* disposing the AudioUnit so
    // we don't get a final fire racing the teardown.
    rateWatcher?.dispose()
    rateWatcher = nil

    if let block = aliveListenerBlock, let deviceID = openedDeviceID {
      var address = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyDeviceIsAlive,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain)
      AudioObjectRemovePropertyListenerBlock(deviceID, &address, .main, block)
      aliveListenerBlock = nil
    }

    if let unit = audioUnit {
      AudioOutputUnitStop(unit)
      AudioComponentInstanceDispose(unit)
      audioUnit = nil
    }
    // Release hog mode if we acquired it. Without this the device
    // stays exclusively held by our PID until the process exits,
    // and any subsequent `open()` from any client (including us)
    // fails with `kAudioHardwareNotRunningError`.
    if didAcquireHogMode, let deviceID = openedDeviceID {
      var pid: pid_t = -1
      var addr = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyHogMode,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
      )
      AudioObjectSetPropertyData(
        deviceID, &addr, 0, nil,
        UInt32(MemoryLayout<pid_t>.size), &pid
      )
      didAcquireHogMode = false
    }
    openedDeviceID = nil
    logger.info("CoreAudio playback closed")
  }

  /// List available playback devices.
  public static func listDevices() -> [(id: AudioDeviceID, name: String)] {
    CoreAudioDevice.devices(scope: .output)
  }
}

/// CoreAudio render callback for playback. Hot path: must not lock,
/// allocate, or call into Swift runtime in a way that could block.
private func playbackCallback(
  inRefCon: UnsafeMutableRawPointer,
  ioActionFlags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
  inTimeStamp: UnsafePointer<AudioTimeStamp>,
  inBusNumber: UInt32,
  inNumberFrames: UInt32,
  ioData: UnsafeMutablePointer<AudioBufferList>?
) -> OSStatus {
  let playback = Unmanaged<CoreAudioPlayback>.fromOpaque(inRefCon).takeUnretainedValue()
  guard let bufferList = ioData else { return noErr }
  let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
  let frameCount = Int(inNumberFrames)

  for (ch, buffer) in buffers.enumerated() {
    guard let data = buffer.mData else { continue }
    let floatPtr = data.assumingMemoryBound(to: Float.self)
    if ch < playback.channels {
      // Drain whatever's queued for this channel; pad the remainder
      // with silence on underrun.
      let copied = playback.playbackRings[ch].consume(into: floatPtr, count: frameCount)
      if copied < frameCount {
        var zero: Float = 0
        vDSP_vfill(&zero, floatPtr + copied, 1, vDSP_Length(frameCount - copied))
      }
    } else {
      // Channel beyond what the engine produces — output silence.
      var zero: Float = 0
      vDSP_vfill(&zero, floatPtr, 1, vDSP_Length(frameCount))
    }
  }
  return noErr
}

// Note: The C callback accesses CoreAudioPlayback properties
// directly since they are declared as internal/public.
