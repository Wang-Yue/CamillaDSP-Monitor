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

import Accelerate
import AudioToolbox
import CoreAudio
import DSPAudio
import DSPBackend
import DSPConfig
import Foundation

public enum SweepRecorder {

  public enum CaptureError: Error, CustomStringConvertible {
    case engineStartFailed(String)
    case noInputNode
    case formatMismatch(String)
    case deviceBindFailed(String)
    case captureBufferEmpty
    case alignmentFailed
    case permissionDenied
    case timeout

    public var description: String {
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

  public struct Result: Sendable {
    /// Trimmed mono recording at the requested `sampleRate`,
    /// time-aligned so sample 0 corresponds to the start of the
    /// played sweep.
    public let captured: [Double]
    /// Estimated round-trip latency in samples (based on
    /// cross-correlation with the original sweep). Diagnostic only.
    public let roundTripSamples: Int
    /// Peak absolute level the mic captured during the sweep. Useful
    /// for warning users about clipping or low signal.
    public let peakAbsolute: Double
  }

  /// Play a sweep + record. `sweep` is the time-domain sweep buffer
  /// at `sampleRate`; `inverse` is the matched Farina inverse used
  /// for cross-correlation alignment (we use `inverse` rather than
  /// `sweep` because the cross-correlation peak is sharper).
  ///
  /// Throws `CaptureError` on failure. Returns once the capture
  /// completes (i.e., after the sweep has played through and a
  /// short tail has been recorded).
  public static func capture(
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
    let outChannels = CoreAudioCapabilities.channelCount(
      deviceName: outputDeviceName, isCapture: false)
    let usableOutChannels = max(1, outChannels)
    let routeChannels = max(2, max(usableOutChannels, outputChannel + 1))

    let leadSamples = Int(leadingSilenceSeconds * Double(sampleRate))
    let tailSamples = Int(trailingSilenceSeconds * Double(sampleRate))
    let totalPlaySamples = leadSamples + sweep.count + tailSamples
    let gain = Double(pow(10.0, playbackGainDB / 20.0))

    // Pre-generate the full time-domain sweep for every route channel
    var playData = [[Double]](
      repeating: [Double](repeating: 0, count: totalPlaySamples), count: routeChannels)
    let targetChannels: Range<Int> =
      outputChannel < 0
      ? 0..<routeChannels
      : {
        let c = max(0, min(outputChannel, routeChannels - 1))
        return c..<(c + 1)
      }()
    for ch in targetChannels {
      for i in 0..<sweep.count {
        playData[ch][leadSamples + i] = sweep[i] * gain
      }
    }

    let playbackConfig = PlaybackDeviceConfig(
      type: .coreAudio,
      channels: routeChannels,
      device: outputDeviceName)

    let playbackBackend = CoreAudioPlayback(
      config: playbackConfig,
      sampleRate: sampleRate,
      chunkSize: 4096)

    do {
      try playbackBackend.open()
    } catch {
      throw CaptureError.engineStartFailed(
        "Could not open playback device: \(error.localizedDescription)")
    }
    defer { playbackBackend.close() }

    let inChannels = CoreAudioCapabilities.channelCount(
      deviceName: inputDeviceName, isCapture: true)
    let usableInChannels = max(1, inChannels)
    let captureConfig = CaptureDeviceConfig(
      type: .coreAudio,
      channels: usableInChannels,
      device: inputDeviceName)

    let captureBackend = CoreAudioCapture(
      config: captureConfig,
      sampleRate: sampleRate,
      chunkSize: 4096)

    do {
      try captureBackend.open()
    } catch {
      throw CaptureError.engineStartFailed(
        "Could not open capture device: \(error.localizedDescription)")
    }
    defer { captureBackend.close() }

    let recorded = RecordingSink(
      targetSampleRate: Double(sampleRate),
      channels: usableInChannels,
      inputChannel: inputChannel)

    let totalDurationSeconds =
      leadingSilenceSeconds + Double(sweep.count) / Double(sampleRate)
      + trailingSilenceSeconds
    let startTimestamp = Date()

    var inChunk = AudioChunk(frames: 4096, channels: usableInChannels)
    var outChunk = AudioChunk(frames: 4096, channels: routeChannels)
    var playCursor = 0

    // Stream playback slices and accumulate capture chunks simultaneously
    while Date().timeIntervalSince(startTimestamp) < totalDurationSeconds + 0.3 {
      // Push playback frames if space is available in the backend SPSC ring
      let freeSpace = 32768 - playbackBackend.bufferLevel
      if freeSpace >= 4096, playCursor < totalPlaySamples {
        let toWrite = min(4096, totalPlaySamples - playCursor)
        for ch in 0..<routeChannels {
          let dst = outChunk[ch]
          let src = playData[ch]
          for i in 0..<toWrite {
            dst[i] = src[playCursor + i]
          }
        }
        outChunk.validFrames = toWrite
        try? playbackBackend.write(chunk: outChunk)
        playCursor += toWrite
      }

      // Pull capture frames if available
      if try captureBackend.read(frames: 4096, into: &inChunk) {
        recorded.append(chunk: inChunk)
      } else {
        try await Task.sleep(nanoseconds: 5_000_000)
      }
    }

    let captured = recorded.snapshot()
    if captured.isEmpty {
      throw CaptureError.captureBufferEmpty
    }

    // Cross-correlate with the inverse sweep to find where the
    // played sweep starts in the recording. The inverse has a sharp
    // autocorrelation peak (that's the whole point of Farina's
    // method), so we get sub-buffer alignment for free.
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
}

// MARK: - Recording sink

/// Thread-safe accumulator for input chunks. The engine's capture loop
/// calls `append`; the main thread calls `snapshot` once playback completes.
private final class RecordingSink: @unchecked Sendable {
  private let lock = NSLock()
  private var buffer: [Double] = []
  private let inputChannel: Int

  init(targetSampleRate: Double, channels: Int, inputChannel: Int) {
    self.inputChannel = max(0, min(inputChannel, max(channels - 1, 0)))
    buffer.reserveCapacity(Int(targetSampleRate * 30))
  }

  func append(chunk: AudioChunk) {
    let frames = chunk.validFrames
    if frames <= 0 { return }
    let chCount = chunk.channels
    let ch = max(0, min(inputChannel, chCount - 1))
    let srcPtr = chunk[ch]
    lock.lock()
    defer { lock.unlock() }
    for i in 0..<frames {
      buffer.append(srcPtr[i])
    }
  }

  func snapshot() -> [Double] {
    lock.lock()
    defer { lock.unlock() }
    return buffer
  }
}
