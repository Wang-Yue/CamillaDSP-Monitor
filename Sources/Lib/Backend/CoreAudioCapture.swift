// CoreAudio capture backend for macOS
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
import DSPAudio
import DSPConfig
import DSPLogging
import Foundation
import Synchronization

public final class CoreAudioCapture: CaptureBackend {
  fileprivate let logger = Logger(label: "dsp.coreaudio.capture")
  private let deviceName: String?
  let channels: Int
  private let sampleRate: Double
  let chunkSize: Int

  var audioUnit: AudioUnit?
  /// Per-channel SPSC ring buffer of `Float` samples. Render callback
  /// is the producer; `read(frames:)` is the consumer.
  let captureRings: [SPSCAudioRingBuffer]
  /// Capacity (samples per channel) the rings were sized for.
  /// Whether the audio unit delivers interleaved or non-interleaved
  /// audio. Determined in `open()`; read by the render callback.
  var isInterleaved = false
  /// Preallocated AudioBufferList + raw per-buffer storage. Filled in
  /// `open()` after the stream format is known, freed in `close()`.
  /// The render callback re-uses these every invocation — no
  /// allocations on the audio thread.
  var preallocBufferList: UnsafeMutablePointer<AudioBufferList>?
  var preallocChannelDataPointers: UnsafeMutableBufferPointer<UnsafeMutableRawPointer>?
  var preallocBytesPerChannelBuffer: Int = 0
  var callbackErrorCount = 0

  /// HAL device the unit is bound to. Captured during `open()` so
  /// `close()` can dispose the rate-change listener without redoing
  /// the lookup (which would race a default-device change).
  private var openedDeviceID: AudioDeviceID?
  /// Watches the device's nominal sample rate so the engine can
  /// surface `.captureFormatChange` when something else flips the
  /// device rate at runtime. `nil` until `open()` resolves a device.
  private var rateWatcher: RateChangeWatcher?
  /// `true` once `open()` has confirmed the device exposes the
  /// "Internal Adjustable" clock source (BlackHole 0.5.0+) and
  /// successfully selected it. Read by the rate-adjust loop to
  /// decide whether to route corrections to `setPitch(_:)` (the
  /// bit-perfect path) or to fall back to the resampler ratio.
  private var pitchControlActive = false

  private let _isDeviceAlive = Atomic<Bool>(true)
  private var aliveListenerBlock: AudioObjectPropertyListenerBlock?

  public var pendingRateChange: Double? { rateWatcher?.pendingRateChange }
  public var pitchControlSupported: Bool { pitchControlActive }
  public func setPitch(_ multiplier: Double) {
    guard pitchControlActive, let id = openedDeviceID else { return }
    CoreAudioDevice.setDevicePitch(id, pitch: multiplier)
  }

  /// Float scratch used by `read(frames:)` to copy samples out of the
  /// SPSC ring before they're widened to `Double` for the AudioChunk.
  /// Sized to one chunk; reused on every read so the consumer thread
  /// doesn't churn the heap.
  private var readScratch: UnsafeMutableBufferPointer<Float>?

  public init(config: CaptureDeviceConfig, sampleRate: Int, chunkSize: Int) {
    self.deviceName = config.device
    self.channels = config.channels
    self.sampleRate = Double(sampleRate)
    self.chunkSize = chunkSize
    // Four chunks of headroom matches the original lock-based design.
    self.captureRings = (0..<config.channels).map { _ in
      SPSCAudioRingBuffer(minimumCapacity: chunkSize * 4)
    }
  }

  deinit {
    // If a caller forgot to call `close()` and dropped us with the
    // AudioUnit still live, the render callback could be running
    // when we free the preallocated buffers — a use-after-free.
    // `close()` calls `AudioOutputUnitStop` which blocks until the
    // current invocation drains, so call it here as a backstop.
    if audioUnit != nil { close() }
    readScratch?.deallocate()
  }

  public func open() throws {
    // If anything below throws after we've started wiring up the
    // AudioUnit, tear it down so we don't leak the HAL handle.
    var openSucceeded = false
    defer { if !openSucceeded { close() } }

    var desc = AudioComponentDescription(
      componentType: kAudioUnitType_Output,
      componentSubType: kAudioUnitSubType_HALOutput,
      componentManufacturer: kAudioUnitManufacturer_Apple,
      componentFlags: 0,
      componentFlagsMask: 0
    )

    guard let component = AudioComponentFindNext(nil, &desc) else {
      throw BackendError.deviceNotFound("No HAL output component found")
    }

    var unit: AudioUnit?
    var status = AudioComponentInstanceNew(component, &unit)
    guard status == noErr, let audioUnit = unit else {
      throw BackendError.initializationFailed("Failed to create AudioUnit: \(status)")
    }
    self.audioUnit = audioUnit

    // Enable input (capture)
    var enableInput: UInt32 = 1
    status = AudioUnitSetProperty(
      audioUnit,
      kAudioOutputUnitProperty_EnableIO,
      kAudioUnitScope_Input,
      1,  // input bus
      &enableInput,
      UInt32(MemoryLayout<UInt32>.size)
    )
    guard status == noErr else {
      throw BackendError.initializationFailed("Failed to enable input: \(status)")
    }

    // Disable output bus on the HAL output unit. Failure here would
    // leave the unit asking the IO bus 0 for render data on top of
    // the input bus, breaking capture silently.
    var disableOutput: UInt32 = 0
    status = AudioUnitSetProperty(
      audioUnit,
      kAudioOutputUnitProperty_EnableIO,
      kAudioUnitScope_Output,
      0,  // output bus
      &disableOutput,
      UInt32(MemoryLayout<UInt32>.size)
    )
    guard status == noErr else {
      throw BackendError.initializationFailed("Failed to disable output bus: \(status)")
    }

    // Resolve the device ID — either the named device or the system
    // default — so we can push our configured sample rate before
    // wiring the AudioUnit. Without this the device stays at its
    // previous rate and the AudioUnit silently sample-rate-converts,
    // which causes audio to play back at the wrong speed when the
    // user switches the engine's rate at runtime.
    let resolvedDeviceID = CoreAudioDevice.deviceID(forName: deviceName, scope: .input)
    self.openedDeviceID = resolvedDeviceID
    if let deviceID = resolvedDeviceID, deviceName != nil {
      var id = deviceID
      AudioUnitSetProperty(
        audioUnit,
        kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global,
        0,
        &id,
        UInt32(MemoryLayout<AudioDeviceID>.size)
      )
    }
    if let deviceID = resolvedDeviceID {
      if !CoreAudioDevice.setNominalSampleRate(deviceID, sampleRate) {
        logger.warning(
          "Capture device refused %f Hz; AudioUnit will sample-rate convert", .double(sampleRate))
      }

      // Explicitly request the device's buffer size to match our chunkSize
      if !CoreAudioDevice.setBufferFrameSize(deviceID, UInt32(chunkSize), scope: .input) {
        logger.warning("Capture device refused buffer size of %d frames", .int(chunkSize))
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
            self.logger.error("Capture device disconnected!")
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
        logger.warning("Failed to add alive listener: %d", .int(Int(status)))
      }
    }

    // Query the device's native stream format on the input bus.
    // After `setNominalSampleRate` succeeded, this reflects the rate
    // we just pushed; if the device refused, it still reflects the
    // original rate and the AudioUnit will SRC for us.
    var deviceFormat = AudioStreamBasicDescription()
    var formatSize = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
    AudioUnitGetProperty(
      audioUnit,
      kAudioUnitProperty_StreamFormat,
      kAudioUnitScope_Input,
      1,  // input bus
      &deviceFormat,
      &formatSize
    )
    logger.info(
      "Device native format: %fHz, %dch", .double(deviceFormat.mSampleRate),
      .int(Int(deviceFormat.mChannelsPerFrame)))

    // Set our desired stream format on the input bus's output scope
    // using the *engine's* sample rate. The AudioUnit handles SRC if
    // the device couldn't be pushed to this rate.
    var streamFormat = CoreAudioDevice.float32StreamFormat(
      sampleRate: sampleRate,
      channels: channels,
      interleaved: false
    )
    status = AudioUnitSetProperty(
      audioUnit,
      kAudioUnitProperty_StreamFormat,
      kAudioUnitScope_Output,
      1,  // input bus output scope
      &streamFormat,
      UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
    )
    if status != noErr {
      logger.warning(
        "Failed to set non-interleaved format (%d), trying interleaved", .int(Int(status)))
      streamFormat = CoreAudioDevice.float32StreamFormat(
        sampleRate: sampleRate,
        channels: channels,
        interleaved: true
      )
      status = AudioUnitSetProperty(
        audioUnit,
        kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Output,
        1,
        &streamFormat,
        UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
      )
      guard status == noErr else {
        throw BackendError.initializationFailed("Failed to set capture format: \(status)")
      }
    }

    isInterleaved = (streamFormat.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0

    // Explicitly configure the maximum frames per slice the AudioUnit expects
    var maxFrames = UInt32(chunkSize)
    if let deviceID = openedDeviceID,
      let actualSize = CoreAudioDevice.bufferFrameSize(deviceID, scope: .input)
    {
      maxFrames = Swift.max(UInt32(chunkSize), actualSize)
    }
    status = AudioUnitSetProperty(
      audioUnit,
      kAudioUnitProperty_MaximumFramesPerSlice,
      kAudioUnitScope_Global,
      0,
      &maxFrames,
      UInt32(MemoryLayout<UInt32>.size)
    )
    if status != noErr {
      logger.warning("Failed to set MaximumFramesPerSlice on AudioUnit: %d", .int(Int(status)))
    }

    // Preallocate all the storage the render callback needs so it
    // does not allocate or call into the Swift runtime on the audio
    // thread. We size every per-buffer block for the configured
    // chunkSize — CoreAudio honours `kAudioDevicePropertyBufferFrameSize`
    // for HAL units, so callbacks won't ask for more frames than that.
    allocateRenderBuffers()

    // Preallocate the read-side Float scratch as well; same reasoning
    // applies on the processing thread which calls `read(frames:)`.
    if readScratch == nil {
      let buf = UnsafeMutableBufferPointer<Float>.allocate(capacity: chunkSize)
      buf.initialize(repeating: 0)
      readScratch = buf
    }

    // Set input callback
    var callbackStruct = AURenderCallbackStruct(
      inputProc: captureCallback,
      inputProcRefCon: Unmanaged.passUnretained(self).toOpaque()
    )
    status = AudioUnitSetProperty(
      audioUnit,
      kAudioOutputUnitProperty_SetInputCallback,
      kAudioUnitScope_Global,
      0,
      &callbackStruct,
      UInt32(MemoryLayout<AURenderCallbackStruct>.size)
    )
    guard status == noErr else {
      throw BackendError.initializationFailed("Failed to set input callback: \(status)")
    }

    status = AudioUnitInitialize(audioUnit)
    guard status == noErr else {
      throw BackendError.initializationFailed("Failed to initialize AudioUnit: \(status)")
    }

    status = AudioOutputUnitStart(audioUnit)
    guard status == noErr else {
      throw BackendError.initializationFailed("Failed to start AudioUnit: \(status)")
    }

    // Register the rate-change watcher *after* we've pushed our
    // configured rate, so the watcher's expected rate is the one
    // we just confirmed — any future listener fires from a user
    // flipping the device rate in Audio MIDI Setup will compare-unequal
    // and surface as `.captureFormatChange`.
    if let deviceID = resolvedDeviceID,
      CoreAudioDevice.hasNominalSampleRateProperty(deviceID)
    {
      rateWatcher = RateChangeWatcher(deviceID: deviceID, expectedRate: sampleRate)
    }

    // Try to enable BlackHole-style clock-pitch tuning. If the
    // device exposes an "Internal Adjustable" clock source (only
    // BlackHole 0.5.0+ does at present) we activate it; the
    // engine's rate-adjust loop will then route corrections via
    // `setPitch(_:)` for bit-perfect drift compensation rather
    // than nudging the resampler ratio.
    if let deviceID = resolvedDeviceID,
      CoreAudioDevice.selectAdjustableClockSource(deviceID)
    {
      pitchControlActive = true
      logger.info("Capture device supports clock-pitch control (Internal Adjustable selected)")
    }

    openSucceeded = true

    logger.info("CoreAudio capture opened: %dch @ %fHz", .int(channels), .double(sampleRate))
  }

  public func read(frames: Int, into chunk: inout AudioChunk) throws -> Bool {
    guard _isDeviceAlive.load(ordering: .acquiring) else {
      throw BackendError.readError("Capture device disconnected")
    }
    // Wait until every channel has at least `frames` samples queued
    // — the producer fills all channels in lockstep, so checking
    // channel 0 is sufficient.
    guard captureRings[0].availableToRead >= frames else { return false }
    guard let scratch = readScratch, scratch.count >= frames else { return false }

    guard let scratchPtr = scratch.baseAddress else { return false }
    for ch in 0..<channels {
      let n = captureRings[ch].consume(into: scratchPtr, count: frames)
      if let dstPtr = chunk[ch].baseAddress {
        vDSP_vspdp(scratchPtr, 1, dstPtr, 1, vDSP_Length(n))
      }
    }

    chunk.validFrames = frames
    return true
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
    deallocateRenderBuffers()
    openedDeviceID = nil
    logger.info("CoreAudio capture closed")
  }

  // MARK: - Render-callback storage

  /// Allocate the AudioBufferList plus per-buffer raw storage that the
  /// render callback re-uses on every invocation. Caller must have set
  /// `isInterleaved` before this is invoked.
  private func allocateRenderBuffers() {
    deallocateRenderBuffers()

    var bufferFrames = chunkSize
    if let deviceID = openedDeviceID,
      let actualSize = CoreAudioDevice.bufferFrameSize(deviceID, scope: .input)
    {
      bufferFrames = Swift.max(chunkSize, Int(actualSize))
    }

    let numBuffers = isInterleaved ? 1 : channels
    let bytesPerBuffer =
      isInterleaved
      ? bufferFrames * channels * MemoryLayout<Float>.size
      : bufferFrames * MemoryLayout<Float>.size

    let listByteCount = AudioBufferList.sizeInBytes(maximumBuffers: numBuffers)
    let listRaw = UnsafeMutableRawPointer.allocate(byteCount: listByteCount, alignment: 16)
    let listPtr = listRaw.bindMemory(to: AudioBufferList.self, capacity: 1)

    let dataPointers = UnsafeMutableBufferPointer<UnsafeMutableRawPointer>.allocate(
      capacity: numBuffers
    )
    for i in 0..<numBuffers {
      let buf = UnsafeMutableRawPointer.allocate(byteCount: bytesPerBuffer, alignment: 16)
      buf.initializeMemory(as: UInt8.self, repeating: 0, count: bytesPerBuffer)
      dataPointers[i] = buf
    }

    // Initialise the AudioBufferList struct once. The render callback
    // doesn't need to touch `mNumberBuffers` or `mDataByteSize`
    // again — it just refills `mData` regions.
    let abl = UnsafeMutableAudioBufferListPointer(listPtr)
    abl.count = numBuffers
    for i in 0..<numBuffers {
      abl[i] = AudioBuffer(
        mNumberChannels: isInterleaved ? UInt32(channels) : 1,
        mDataByteSize: UInt32(bytesPerBuffer),
        mData: dataPointers[i]
      )
    }

    preallocBufferList = listPtr
    preallocChannelDataPointers = dataPointers
    preallocBytesPerChannelBuffer = bytesPerBuffer
  }

  private func deallocateRenderBuffers() {
    if let dataPointers = preallocChannelDataPointers {
      for i in 0..<dataPointers.count {
        dataPointers[i].deallocate()
      }
      dataPointers.deallocate()
      preallocChannelDataPointers = nil
    }
    if let listPtr = preallocBufferList {
      UnsafeMutableRawPointer(listPtr).deallocate()
      preallocBufferList = nil
    }

    preallocBytesPerChannelBuffer = 0
  }

  // MARK: - Device Enumeration

  /// List available capture devices.
  public static func listDevices() -> [(id: AudioDeviceID, name: String)] {
    CoreAudioDevice.devices(scope: .input)
  }
}

/// CoreAudio render callback for capture. Hot path: must not lock,
/// allocate, or call into Swift runtime in a way that could block.
private func captureCallback(
  inRefCon: UnsafeMutableRawPointer,
  ioActionFlags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
  inTimeStamp: UnsafePointer<AudioTimeStamp>,
  _: UInt32,
  inNumberFrames: UInt32,
  _: UnsafeMutablePointer<AudioBufferList>?
) -> OSStatus {
  let capture = Unmanaged<CoreAudioCapture>.fromOpaque(inRefCon).takeUnretainedValue()
  let channels = capture.channels
  let frameCount = Int(inNumberFrames)
  let interleaved = capture.isInterleaved

  // Use the AudioBufferList + per-buffer storage that was allocated in
  // `open()`. If the unit was started without `open()` somehow, bail.
  guard let bufferListPtr = capture.preallocBufferList,
    let dataPointers = capture.preallocChannelDataPointers,
    let audioUnit = capture.audioUnit
  else { return noErr }

  // `AudioUnitRender` overwrites each buffer's `mDataByteSize` with
  // the bytes actually delivered. If a previous call short-delivered
  // (rare, but observed during HAL hiccups), the next call would see
  // the stale shrunken size and `AudioUnitRender` could refuse to
  // write more than that. Reset every buffer to its preallocated
  // capacity before every render.
  let bufferList = UnsafeMutableAudioBufferListPointer(bufferListPtr)
  let preallocSize = UInt32(capture.preallocBytesPerChannelBuffer)
  for i in 0..<bufferList.count {
    bufferList[i].mDataByteSize = preallocSize
  }

  let status = AudioUnitRender(
    audioUnit, ioActionFlags, inTimeStamp, 1, inNumberFrames, bufferListPtr
  )
  guard status == noErr else {
    if capture.callbackErrorCount < 3 {
      capture.callbackErrorCount += 1
      capture.logger.error("Capture render error: %d", .int(Int(status)))
    }
    return noErr
  }

  // After render, `mDataByteSize` reflects what was actually written.
  // Use that to decide how many frames per ring write — guards
  // against short-frame deliveries reading past valid data.
  let actualFrames: Int
  if interleaved {
    let bytesPerFrame = MemoryLayout<Float>.size * channels
    actualFrames =
      bytesPerFrame > 0
      ? Int(bufferList[0].mDataByteSize) / bytesPerFrame
      : frameCount
  } else {
    let bytesPerFrame = MemoryLayout<Float>.size
    actualFrames = Int(bufferList[0].mDataByteSize) / bytesPerFrame
  }

  if actualFrames < frameCount {
    capture.logger.warning(
      "Capture underrun: expected %d frames, got %d", .int(frameCount), .int(actualFrames))
  }

  let frames = Swift.min(actualFrames, frameCount)
  guard frames > 0 else { return noErr }

  if interleaved {
    // One buffer holds [L0, R0, L1, R1, …]. Pull each channel out
    // with a strided write into its dedicated SPSC ring.
    let floatPtr = dataPointers[0].assumingMemoryBound(to: Float.self)
    for ch in 0..<channels {
      capture.captureRings[ch].write(
        source: floatPtr + ch,
        count: frames,
        stride: channels
      )
    }
  } else {
    // Non-interleaved: one buffer per channel. Direct contiguous
    // copy into each ring.
    for ch in 0..<channels {
      let floatPtr = dataPointers[ch].assumingMemoryBound(to: Float.self)
      capture.captureRings[ch].write(
        source: floatPtr,
        count: frames,
        stride: 1
      )
    }
  }

  return noErr
}

// Note: The C callback accesses CoreAudioCapture properties
// directly since they are declared as internal/public.
