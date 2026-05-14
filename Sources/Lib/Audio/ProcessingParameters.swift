// Concurrency model
// -----------------
// Every field is backed by an `Atomic` from the standard `Synchronization`
// module — no `NSLock`, no `@unchecked Sendable`.
// per-channel level vectors live in a fixed-size lock-free struct sized
// for the engine's stereo-only audio path.

import Synchronization

public final class ProcessingParameters: Sendable {

  /// Default volume (dB) when an engine starts.
  public static let defaultVolume: PrcFmt = 0.0
  /// Default mute state.
  public static let defaultMute = false

  // MARK: - Storage

  /// Target volume (dB) — what the user has asked for. UI thread writes;
  /// VolumeFilter reads on every chunk.
  private let _targetVolume: AtomicDouble
  private let _currentVolume: AtomicDouble
  /// Mute state. UI writes; VolumeFilter reads each chunk.
  private let _muted: Atomic<Bool>

  /// Per-channel signal levels (dB).
  private let _captureSignalPeak: AtomicLevels
  private let _captureSignalRms: AtomicLevels
  private let _playbackSignalPeak: AtomicLevels
  private let _playbackSignalRms: AtomicLevels

  public init(captureChannels: Int, playbackChannels: Int) {
    self._targetVolume = AtomicDouble(Self.defaultVolume)
    self._currentVolume = AtomicDouble(Self.defaultVolume)
    self._muted = Atomic<Bool>(Self.defaultMute)

    self._captureSignalPeak = AtomicLevels(channels: captureChannels)
    self._captureSignalRms = AtomicLevels(channels: captureChannels)
    self._playbackSignalPeak = AtomicLevels(channels: playbackChannels)
    self._playbackSignalRms = AtomicLevels(channels: playbackChannels)
  }

  // MARK: - Volume / Mute

  public var targetVolume: PrcFmt {
    get { _targetVolume.value }
    set { _targetVolume.value = newValue }
  }

  public var currentVolume: PrcFmt {
    get { _currentVolume.value }
    set { _currentVolume.value = newValue }
  }

  public var isMuted: Bool {
    get { _muted.load(ordering: .acquiring) }
    set { _muted.store(newValue, ordering: .releasing) }
  }

  // MARK: - Metrics

  public var captureSignalPeak: [PrcFmt] {
    get { _captureSignalPeak.snapshot }
    set { _captureSignalPeak.store(newValue) }
  }

  public var captureSignalRms: [PrcFmt] {
    get { _captureSignalRms.snapshot }
    set { _captureSignalRms.store(newValue) }
  }

  public var playbackSignalPeak: [PrcFmt] {
    get { _playbackSignalPeak.snapshot }
    set { _playbackSignalPeak.store(newValue) }
  }

  public var playbackSignalRms: [PrcFmt] {
    get { _playbackSignalRms.snapshot }
    set { _playbackSignalRms.store(newValue) }
  }

  // MARK: - Chunk-based updates (no-allocation, audio-thread safe)

  public func updateCaptureLevels(from chunk: AudioChunk) -> PrcFmt {
    return updateLevels(from: chunk, peakStorage: _captureSignalPeak, rmsStorage: _captureSignalRms)
  }

  public func updatePlaybackLevels(from chunk: AudioChunk) -> PrcFmt {
    return updateLevels(
      from: chunk, peakStorage: _playbackSignalPeak, rmsStorage: _playbackSignalRms)
  }

  private func updateLevels(
    from chunk: AudioChunk, peakStorage: AtomicLevels, rmsStorage: AtomicLevels
  ) -> PrcFmt {
    let channelCount = min(chunk.channels, peakStorage.count)
    guard channelCount > 0 else { return -1000.0 }
    let frameCount = chunk.validFrames
    var maxPeak: PrcFmt = -1000.0
    for i in 0..<channelCount {
      let buffer = UnsafeBufferPointer(chunk[i])

      let peakDb = PrcFmt.toDB(DSPOps.peakAbsolute(buffer, count: frameCount))
      peakStorage.levels[i].value = peakDb
      if peakDb > maxPeak {
        maxPeak = peakDb
      }
      let rmsDb = PrcFmt.toDB(DSPOps.rms(buffer, count: frameCount))
      rmsStorage.levels[i].value = rmsDb
    }

    return maxPeak
  }
}

// MARK: - AtomicLevels

/// Lock-free fixed-size `PrcFmt` level storage using an array of `AtomicDouble`.
/// Maintains the same interface but simplifies implementation and removes unsafe pointers.
final class AtomicLevels: Sendable {
  fileprivate let levels: [AtomicDouble]
  let count: Int

  init(channels: Int) {
    self.count = channels
    self.levels = (0..<channels).map { _ in AtomicDouble(-1000.0) }
  }

  /// Publish new values.
  func store(_ values: [PrcFmt]) {
    let limit = min(values.count, count)
    for i in 0..<limit {
      levels[i].value = values[i]
    }
  }

  /// Snapshot the current levels.
  var snapshot: [PrcFmt] {
    return levels.map { $0.value }
  }
}
