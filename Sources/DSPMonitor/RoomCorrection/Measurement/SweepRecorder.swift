// Plays a sweep through the system output while capturing the
// microphone, then returns a time-aligned recording for the
// downstream `SweepDeconvolver`.
//
// Implementation choices:
//
//   - **Native Core Audio Backends** (`CoreAudioPlayback` / `CoreAudioCapture`)
//     rather than `AVAudioEngine`. Decoupling the capture and playback
//     graphs bypasses macOS AUHAL routing-layout validation bugs and
//     silent input stalls. The sweep slices and capture blocks are
//     streamed in lockstep using lock-free SPSC rings, and internal
//     hardware units handle any sample rate conversion automatically.
//
//   - **Cross-correlation alignment** post-capture. The round-trip
//     latency depends on the buffer sizes and device drivers, which
//     vary. Rather than try to predict it, we cross-correlate the
//     recording against the original sweep and trim from the peak.
//
//   - **Silence padding** before / after the sweep. Gives the audio
//     stack room to pre-roll without truncating the leading rise of
//     the sweep, and keeps a clean buffer of room reverb tail to
//     deconvolve.

import AVFoundation
import Accelerate
import AudioToolbox
import CoreAudio
import Foundation

enum SweepRecorder {

  enum CaptureError: Error, CustomStringConvertible {
    case engineStartFailed(String)
    case noInputNode
    case formatMismatch(String)
    case deviceBindFailed(String)
    case captureBufferEmpty
    case alignmentFailed
    case permissionDenied
    case timeout

    var description: String {
      switch self {
      case .engineStartFailed(let m): return "Audio engine start failed: \(m)"
      case .noInputNode: return "Audio engine has no input node."
      case .formatMismatch(let m): return "Format mismatch: \(m)"
      case .deviceBindFailed(let m): return "Could not bind input device: \(m)"
      case .captureBufferEmpty: return "Captured no audio."
      case .alignmentFailed: return "Could not align captured signal with sweep."
      case .permissionDenied: return "Microphone access denied."
      case .timeout: return "Capture timed out before audio arrived."
      }
    }
  }

  struct Result: Sendable {
    /// Trimmed mono recording at the requested `sampleRate`,
    /// time-aligned so sample 0 corresponds to the start of the
    /// played sweep.
    let captured: [Double]
    /// Estimated round-trip latency in samples (based on
    /// cross-correlation with the original sweep). Diagnostic only.
    let roundTripSamples: Int
    /// Peak absolute level the mic captured during the sweep. Useful
    /// for warning users about clipping or low signal.
    let peakAbsolute: Double
  }

  /// Play a sweep + record. `sweep` is the time-domain sweep buffer
  /// at `sampleRate`; `inverse` is the matched Farina inverse used
  /// for cross-correlation alignment (we use `inverse` rather than
  /// `sweep` because the cross-correlation peak is sharper).
  ///
  /// Throws `CaptureError` on failure. Returns once the capture
  /// completes (i.e., after the sweep has played through and a
  /// short tail has been recorded).
  static func capture(
    sweep: [Double],
    inverse: [Double],
    sampleRate: Int,
    inputDeviceName: String? = nil,
    outputDeviceName: String? = nil,
    inputChannel: Int = 0,
    outputChannel: Int = -1,
    leadingSilenceSeconds: Double = 0.5,
    trailingSilenceSeconds: Double = 0.5,
    playbackGainDB: Double = -12.0
  ) async throws -> Result {
    try await ensureMicrophonePermission()

    let outputDeviceID = deviceID(forName: outputDeviceName, isCapture: false)
    let outChannels =
      outputDeviceID != nil ? channelCount(for: outputDeviceID!, isCapture: false) : 2
    let usableOutChannels = max(1, outChannels)
    let routeChannels = max(2, max(usableOutChannels, outputChannel + 1))

    let inputDeviceID = deviceID(forName: inputDeviceName, isCapture: true)

    let leadSamples = Int(leadingSilenceSeconds * Double(sampleRate))
    let tailSamples = Int(trailingSilenceSeconds * Double(sampleRate))
    let totalPlaySamples = leadSamples + sweep.count + tailSamples
    let gain = Double(pow(10.0, playbackGainDB / 20.0))

    let outputFormat = AVAudioFormat(
      commonFormat: .pcmFormatFloat32,
      sampleRate: Double(sampleRate),
      channels: AVAudioChannelCount(routeChannels),
      interleaved: false
    )!

    guard
      let pcmBuffer = AVAudioPCMBuffer(
        pcmFormat: outputFormat, frameCapacity: AVAudioFrameCount(totalPlaySamples))
    else {
      throw CaptureError.engineStartFailed("Could not allocate PCM buffer.")
    }
    pcmBuffer.frameLength = AVAudioFrameCount(totalPlaySamples)

    // Zero out all channels first
    for ch in 0..<routeChannels {
      guard let channelData = pcmBuffer.floatChannelData else { continue }
      let ptr = channelData[ch]
      for i in 0..<totalPlaySamples {
        ptr[i] = 0.0
      }
    }

    // Fill target channels with sweep
    let targetChannels: Range<Int> =
      outputChannel < 0
      ? 0..<routeChannels
      : {
        let c = max(0, min(outputChannel, routeChannels - 1))
        return c..<(c + 1)
      }()

    for ch in targetChannels {
      guard let channelData = pcmBuffer.floatChannelData else { continue }
      let ptr = channelData[ch]
      for i in 0..<sweep.count {
        ptr[i + leadSamples] = Float(sweep[i] * gain)
      }
    }

    let playbackEngine = AVAudioEngine()
    let captureEngine = AVAudioEngine()
    let player = AVAudioPlayerNode()
    playbackEngine.attach(player)

    // Set current devices on input/output nodes
    if let inputDeviceID {
      var id = inputDeviceID
      let inputUnit = captureEngine.inputNode.audioUnit!
      AudioUnitSetProperty(
        inputUnit,
        kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global,
        0,
        &id,
        UInt32(MemoryLayout<AudioDeviceID>.size)
      )
    }

    if let outputDeviceID {
      var id = outputDeviceID
      let outputUnit = playbackEngine.outputNode.audioUnit!
      AudioUnitSetProperty(
        outputUnit,
        kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global,
        0,
        &id,
        UInt32(MemoryLayout<AudioDeviceID>.size)
      )
    }

    let inputMixer = AVAudioMixerNode()
    captureEngine.attach(inputMixer)

    var recordedSamples = [Double]()
    let lock = NSLock()

    func nominalRate(for id: AudioDeviceID) -> Double? {
      var addr = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyNominalSampleRate,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
      )
      var val = Double(0)
      var size = UInt32(MemoryLayout<Double>.size)
      if AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &val) == noErr {
        return val
      }
      return nil
    }

    let hwInput = captureEngine.inputNode.outputFormat(forBus: 0)
    let outputDeviceRate = outputDeviceID.flatMap(nominalRate)

    // Explicitly connect inputNode to inputMixer on the capture engine
    captureEngine.connect(captureEngine.inputNode, to: inputMixer, format: hwInput)

    // Connect player to mainMixerNode, and mainMixerNode to outputNode on the playback engine
    playbackEngine.connect(player, to: playbackEngine.mainMixerNode, format: outputFormat)
    let nominalOutputRate = outputDeviceRate ?? Double(sampleRate)
    let nominalOutputFormat = AVAudioFormat(
      commonFormat: .pcmFormatFloat32,
      sampleRate: nominalOutputRate,
      channels: AVAudioChannelCount(routeChannels),
      interleaved: false
    )!
    playbackEngine.connect(
      playbackEngine.mainMixerNode, to: playbackEngine.outputNode, format: nominalOutputFormat)

    do {
      try playbackEngine.start()
    } catch {
      throw CaptureError.engineStartFailed("Playback engine: \(error.localizedDescription)")
    }

    do {
      try captureEngine.start()
    } catch {
      playbackEngine.stop()
      throw CaptureError.engineStartFailed("Capture engine: \(error.localizedDescription)")
    }

    if Int(hwInput.sampleRate) != sampleRate {
      playbackEngine.stop()
      captureEngine.stop()
      throw CaptureError.formatMismatch(
        "Input device sample rate (\(Int(hwInput.sampleRate)) Hz) must match measurement sample rate (\(sampleRate) Hz). Please check Audio MIDI Setup."
      )
    }

    // Tap the mixer node on the capture engine
    inputMixer.installTap(onBus: 0, bufferSize: 4096, format: hwInput) { buffer, time in
      guard let channelData = buffer.floatChannelData else { return }
      let frames = Int(buffer.frameLength)
      let ch = max(0, min(inputChannel, Int(buffer.format.channelCount) - 1))
      let ptr = channelData[ch]

      lock.withLock {
        for i in 0..<frames {
          recordedSamples.append(Double(ptr[i]))
        }
      }
    }

    player.play()
    await player.scheduleBuffer(pcmBuffer)

    let tailNanoseconds = UInt64((trailingSilenceSeconds + 0.5) * 1_000_000_000)
    try? await Task.sleep(nanoseconds: tailNanoseconds)

    player.stop()
    playbackEngine.stop()
    captureEngine.stop()
    inputMixer.removeTap(onBus: 0)

    let captured = lock.withLock { recordedSamples }

    if captured.isEmpty {
      throw CaptureError.captureBufferEmpty
    }

    // Cross-correlate with the inverse sweep to find where the
    // played sweep starts in the recording.
    let alignmentSamples = locateSweepStart(
      in: captured, inverse: inverse)
    guard let startSample = alignmentSamples else {
      throw CaptureError.alignmentFailed
    }

    let trimmed = trimAndAlign(
      captured: captured,
      startSample: startSample,
      sweepLength: sweep.count,
      tailSamples: tailSamples)
    let peak = trimmed.map { abs($0) }.max() ?? 0
    return Result(
      captured: trimmed,
      roundTripSamples: max(0, startSample - leadSamples),
      peakAbsolute: peak)
  }

  // MARK: - Internals

  /// Cross-correlate `recording` with `inverse`. The peak of the
  /// resulting signal corresponds to where the original sweep
  /// "starts" in the recording — that's our alignment marker.
  ///
  /// The inverse-filter trick: convolving the recorded sweep with
  /// the matched Farina inverse approximates a Dirac, so the peak
  /// is sharp and well-localised even on noisy mic captures. We
  /// reuse `SweepDeconvolver.convolve` for the actual math.
  private static func locateSweepStart(
    in recording: [Double], inverse: [Double]
  ) -> Int? {
    let convolved = SweepDeconvolver.convolve(recording, with: inverse)
    var peakIdx = 0
    var peakAbs = 0.0
    for i in 0..<convolved.count {
      let v = abs(convolved[i])
      if v > peakAbs {
        peakAbs = v
        peakIdx = i
      }
    }
    if peakAbs <= 0 { return nil }
    // The convolution peak lands at `inverse.count - 1 + sweepStart`
    // (since `convolve` returns lengths summed). Shift back to recover
    // the sweep's start sample in the recording.
    return peakIdx - (inverse.count - 1)
  }

  /// Slice `captured[startSample ..< startSample + sweepLength + tail]`,
  /// padding with zeros if the recording cut off early. The tail is
  /// kept so the deconvolver sees the room's full decay.
  private static func trimAndAlign(
    captured: [Double], startSample: Int, sweepLength: Int, tailSamples: Int
  ) -> [Double] {
    let needed = sweepLength + tailSamples
    var out = [Double](repeating: 0, count: needed)
    for i in 0..<needed {
      let srcIdx = startSample + i
      if srcIdx >= 0 && srcIdx < captured.count {
        out[i] = captured[srcIdx]
      }
    }
    return out
  }

  private static func ensureMicrophonePermission() async throws {
    let status = AVCaptureDevice.authorizationStatus(for: .audio)
    if status == .denied || status == .restricted {
      throw CaptureError.permissionDenied
    }
    if status == .notDetermined {
      let granted = await withCheckedContinuation {
        (continuation: CheckedContinuation<Bool, Never>) in
        AVCaptureDevice.requestAccess(for: .audio) { response in
          continuation.resume(returning: response)
        }
      }
      if !granted {
        throw CaptureError.permissionDenied
      }
    }
  }

  private static func deviceID(forName name: String?, isCapture: Bool) -> AudioDeviceID? {
    guard let name = name, !name.isEmpty else {
      var addr = AudioObjectPropertyAddress(
        mSelector: isCapture
          ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
      )
      var id = AudioDeviceID(0)
      var size = UInt32(MemoryLayout<AudioDeviceID>.size)
      if AudioObjectGetPropertyData(
        AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &id) == noErr
      {
        return id
      }
      return nil
    }

    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioHardwarePropertyDevices,
      mScope: kAudioObjectPropertyScopeGlobal,
      mElement: kAudioObjectPropertyElementMain
    )
    var size: UInt32 = 0
    guard
      AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size)
        == noErr, size > 0
    else {
      return nil
    }
    let count = Int(size) / MemoryLayout<AudioDeviceID>.size
    var ids = [AudioDeviceID](repeating: 0, count: count)
    guard
      AudioObjectGetPropertyData(
        AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr
    else {
      return nil
    }

    for id in ids {
      var nameAddr = AudioObjectPropertyAddress(
        mSelector: kAudioObjectPropertyName,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
      )
      var devName: Unmanaged<CFString>?
      var nameSize = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
      if AudioObjectGetPropertyData(id, &nameAddr, 0, nil, &nameSize, &devName) == noErr,
        let cfName = devName?.takeRetainedValue() as String?,
        cfName == name
      {
        return id
      }
    }
    return nil
  }

  private static func channelCount(for deviceID: AudioDeviceID, isCapture: Bool) -> Int {
    var addr = AudioObjectPropertyAddress(
      mSelector: kAudioDevicePropertyStreamConfiguration,
      mScope: isCapture ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
      mElement: kAudioObjectPropertyElementMain
    )
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(deviceID, &addr, 0, nil, &size) == noErr, size > 0 else {
      return 0
    }
    let bufferList = UnsafeMutablePointer<AudioBufferList>.allocate(capacity: Int(size))
    defer { bufferList.deallocate() }
    guard AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &size, bufferList) == noErr else {
      return 0
    }
    let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
    var channels = 0
    for buffer in buffers {
      channels += Int(buffer.mNumberChannels)
    }
    return channels
  }
}
