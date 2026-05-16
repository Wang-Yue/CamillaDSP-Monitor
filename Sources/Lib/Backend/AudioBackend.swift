// Audio backend protocols.
//
// `ProcessingState` and `ProcessingStopReason` — used by both the
// engine internals and the public actor — live in `Engine/DSPEngine.swift`.

import DSPAudio
import DSPConfig
import Foundation

public func createCaptureBackend(config: CaptureDeviceConfig, sampleRate: Int, chunkSize: Int)
  throws -> CaptureBackend
{
  switch config.type {
  case .coreAudio:
    return CoreAudioCapture(config: config, sampleRate: sampleRate, chunkSize: chunkSize)
  }
}

public func createPlaybackBackend(config: PlaybackDeviceConfig, sampleRate: Int, chunkSize: Int)
  throws -> PlaybackBackend
{
  switch config.type {
  case .coreAudio:
    return CoreAudioPlayback(config: config, sampleRate: sampleRate, chunkSize: chunkSize)
  }
}

/// Protocol for audio capture backends
public protocol CaptureBackend: AnyObject {
  /// Open the capture device
  func open() throws
  /// Read a chunk of audio into the provided buffer. Returns false on end-of-stream or no data.
  func read(frames: Int, into chunk: inout AudioChunk) throws -> Bool
  /// Close the capture device
  func close()

  /// New nominal sample rate detected on the device since `open()`,
  /// or `nil` if the rate is still the one we asked for. Polled by
  /// the engine each chunk to surface
  /// `ProcessingStopReason.captureFormatChange` when a user (or
  /// another app) flips the device rate at runtime.
  var pendingRateChange: Double? { get }
  /// `true` when the capture device exposes a tunable clock — at
  /// the moment that's BlackHole 0.5.0+ on macOS, which advertises
  /// an "Internal Adjustable" clock source. When `true`, the
  /// rate-adjust loop sends corrections through `setPitch(_:)`
  /// instead of nudging the resampler ratio (bit-perfect path).
  var pitchControlSupported: Bool { get }
  /// Apply a clock-pitch correction to the capture device.
  /// `multiplier` is close to `1.0` (typically `1.0 ± 0.001`).
  /// No-op for backends without tunable clocks.
  func setPitch(_ multiplier: Double)
}

extension CaptureBackend {
  public var pendingRateChange: Double? { nil }
  public var pitchControlSupported: Bool { false }
  public func setPitch(_ multiplier: Double) {}
}

/// Protocol for audio playback backends
public protocol PlaybackBackend: AnyObject {
  /// Open the playback device
  func open() throws
  /// Write a chunk of audio
  func write(chunk: AudioChunk) throws
  /// Close the playback device
  func close()
  /// Get the current playback buffer level in samples
  var bufferLevel: Int { get }
  /// See `CaptureBackend.pendingRateChange`. Used to surface
  /// `ProcessingStopReason.playbackFormatChange`.
  var pendingRateChange: Double? { get }
  /// Push `frames` zero samples per channel into the output ring
  /// before the engine's first real chunk arrives. Used at startup
  /// so the rate-adjust controller sees a buffer level near
  /// `target_level` from its first measurement, instead of having
  /// to ramp up from empty.
  func prefillSilence(frames: Int) throws
}

extension PlaybackBackend {
  public var pendingRateChange: Double? { nil }
  public func prefillSilence(frames: Int) throws {}
}
