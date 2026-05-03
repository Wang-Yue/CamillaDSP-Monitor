// Lock-free shared processing state. Holds the parameters that the
// VolumeFilter, Pipeline, and DSPEngine actor need to read and write
// across the engine's capture/processing/playback threads.
//
// Concurrency model
// -----------------
// Every field is backed by an `Atomic` from the standard `Synchronization`
// module — no `NSLock`, no `@unchecked Sendable`. Doubles are bit-cast to
// `UInt64` (loss-less and lock-free on every platform Swift 6 supports);
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
  private let _targetVolumeBits: Atomic<UInt64>
  /// Current ramped volume (dB) — what the filter is actually applying.
  /// VolumeFilter writes after each chunk; UI thread may display.
  private let _currentVolumeBits: Atomic<UInt64>
  /// Mute state. UI writes; VolumeFilter reads each chunk.
  private let _muted: Atomic<Bool>
  /// Processing load percentage (0–100), as a Double bit-pattern.
  private let _processingLoadBits: Atomic<UInt64>

  /// Per-channel signal levels (dB).
  private let _captureSignalPeak: AtomicLevels
  private let _captureSignalRms: AtomicLevels
  private let _playbackSignalPeak: AtomicLevels
  private let _playbackSignalRms: AtomicLevels

  public init(captureChannels: Int, playbackChannels: Int) {
    self._targetVolumeBits = Atomic<UInt64>(Self.defaultVolume.bitPattern)
    self._currentVolumeBits = Atomic<UInt64>(Self.defaultVolume.bitPattern)
    self._muted = Atomic<Bool>(Self.defaultMute)
    self._processingLoadBits = Atomic<UInt64>(Double(0).bitPattern)
    self._captureSignalPeak = AtomicLevels(channels: captureChannels)
    self._captureSignalRms = AtomicLevels(channels: captureChannels)
    self._playbackSignalPeak = AtomicLevels(channels: playbackChannels)
    self._playbackSignalRms = AtomicLevels(channels: playbackChannels)
  }

  // MARK: - Volume / Mute

  public var targetVolume: PrcFmt {
    get { Double(bitPattern: _targetVolumeBits.load(ordering: .acquiring)) }
    set { _targetVolumeBits.store(newValue.bitPattern, ordering: .releasing) }
  }

  public var currentVolume: PrcFmt {
    get { Double(bitPattern: _currentVolumeBits.load(ordering: .acquiring)) }
    set { _currentVolumeBits.store(newValue.bitPattern, ordering: .releasing) }
  }

  public var isMuted: Bool {
    get { _muted.load(ordering: .acquiring) }
    set { _muted.store(newValue, ordering: .releasing) }
  }

  // MARK: - Metrics

  public var processingLoad: Double {
    get { Double(bitPattern: _processingLoadBits.load(ordering: .acquiring)) }
    set { _processingLoadBits.store(newValue.bitPattern, ordering: .releasing) }
  }

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

  // MARK: Multi-channel setters

  public func setCaptureSignalPeak(_ values: [PrcFmt]) {
    _captureSignalPeak.store(values)
  }
  public func setCaptureSignalRms(_ values: [PrcFmt]) {
    _captureSignalRms.store(values)
  }
  public func setPlaybackSignalPeak(_ values: [PrcFmt]) {
    _playbackSignalPeak.store(values)
  }
  public func setPlaybackSignalRms(_ values: [PrcFmt]) {
    _playbackSignalRms.store(values)
  }

  // Convenience for stereo if needed by legacy code or specific call sites
  public func setCaptureSignalPeak(left: PrcFmt, right: PrcFmt) {
    _captureSignalPeak.store([left, right])
  }
  public func setCaptureSignalRms(left: PrcFmt, right: PrcFmt) {
    _captureSignalRms.store([left, right])
  }
  public func setPlaybackSignalPeak(left: PrcFmt, right: PrcFmt) {
    _playbackSignalPeak.store([left, right])
  }
  public func setPlaybackSignalRms(left: PrcFmt, right: PrcFmt) {
    _playbackSignalRms.store([left, right])
  }

  // MARK: - Chunk-based updates (no-allocation, audio-thread safe)

  public func updateCaptureLevels(from chunk: AudioChunk) -> PrcFmt {
    let count = min(chunk.channels, _captureSignalPeak.count)
    guard count > 0 else { return -1000.0 }

    var loudest: PrcFmt = -1000.0
    for i in 0..<count {
      let peak = PrcFmt.toDB(DSPOps.peakAbsolute(chunk.waveforms[i]))
      if peak > loudest { loudest = peak }
      _captureSignalPeak.store(channel: i, value: peak, ordering: .relaxed)

      let rms = PrcFmt.toDB(DSPOps.rms(chunk.waveforms[i]))
      _captureSignalRms.store(channel: i, value: rms, ordering: .relaxed)
    }
    // Fence on last one
    let lastIdx = count - 1
    let lastPeak = PrcFmt.toDB(DSPOps.peakAbsolute(chunk.waveforms[lastIdx]))
    _captureSignalPeak.store(channel: lastIdx, value: lastPeak, ordering: .releasing)

    let lastRms = PrcFmt.toDB(DSPOps.rms(chunk.waveforms[lastIdx]))
    _captureSignalRms.store(channel: lastIdx, value: lastRms, ordering: .releasing)

    return loudest
  }

  public func updatePlaybackLevels(from chunk: AudioChunk) -> PrcFmt {
    let count = min(chunk.channels, _playbackSignalPeak.count)
    guard count > 0 else { return -1000.0 }

    var loudest: PrcFmt = -1000.0
    for i in 0..<count {
      let peak = PrcFmt.toDB(DSPOps.peakAbsolute(chunk.waveforms[i]))
      if peak > loudest { loudest = peak }
      _playbackSignalPeak.store(channel: i, value: peak, ordering: .relaxed)

      let rms = PrcFmt.toDB(DSPOps.rms(chunk.waveforms[i]))
      _playbackSignalRms.store(channel: i, value: rms, ordering: .relaxed)
    }
    // Fence on last one
    let lastIdx = count - 1
    let lastPeak = PrcFmt.toDB(DSPOps.peakAbsolute(chunk.waveforms[lastIdx]))
    _playbackSignalPeak.store(channel: lastIdx, value: lastPeak, ordering: .releasing)

    let lastRms = PrcFmt.toDB(DSPOps.rms(chunk.waveforms[lastIdx]))
    _playbackSignalRms.store(channel: lastIdx, value: lastRms, ordering: .releasing)

    return loudest
  }
}

// MARK: - AtomicLevels

/// Lock-free fixed stereo (left + right) `PrcFmt` level pair. Two
/// inline `Atomic<UInt64>` slots holding the IEEE-754 bit patterns.
///
/// Mono input is mirrored to both sides on `store`; anything beyond the
/// first two channels is ignored. The right-channel store carries the
/// release fence, so a reader that does an acquire-load on right is
/// guaranteed to also see the matching left write.
final class AtomicLevels: @unchecked Sendable {

  private let ptr: UnsafeMutablePointer<Atomic<UInt64>>
  public let count: Int

  init(channels: Int) {
    self.count = channels
    self.ptr = UnsafeMutablePointer<Atomic<UInt64>>.allocate(capacity: channels)
    let silentBits = Double(-1000.0).bitPattern
    for i in 0..<channels {
      ptr.advanced(by: i).initialize(to: Atomic<UInt64>(silentBits))
    }
  }

  deinit {
    for i in 0..<count {
      ptr.advanced(by: i).deinitialize(count: 1)
    }
    ptr.deallocate()
  }

  /// Publish new values.
  func store(_ values: [PrcFmt]) {
    guard count > 0 else { return }
    let limit = min(values.count, count)
    for i in 0..<limit {
      ptr.advanced(by: i).pointee.store(values[i].bitPattern, ordering: .relaxed)
    }
    // Apply release fence on the last written element to ensure visibility of others
    if limit > 0 {
      let lastIdx = limit - 1
      let val = values[lastIdx]
      ptr.advanced(by: lastIdx).pointee.store(val.bitPattern, ordering: .releasing)
    }
  }

  /// Store a value for a specific channel with given ordering.
  func store(channel: Int, value: PrcFmt, ordering: AtomicStoreOrdering) {
    guard channel < count else { return }
    switch ordering {
    case .relaxed:
      ptr.advanced(by: channel).pointee.store(value.bitPattern, ordering: .relaxed)
    case .releasing:
      ptr.advanced(by: channel).pointee.store(value.bitPattern, ordering: .releasing)
    default:
      fatalError("Unsupported ordering")
    }
  }

  /// Snapshot the current levels.
  var snapshot: [PrcFmt] {
    guard count > 0 else { return [] }
    let lastIdx = count - 1
    let lastVal = Double(bitPattern: ptr.advanced(by: lastIdx).pointee.load(ordering: .acquiring))

    var result = [PrcFmt](repeating: 0, count: count)
    result[lastIdx] = lastVal

    for i in 0..<lastIdx {
      result[i] = Double(bitPattern: ptr.advanced(by: i).pointee.load(ordering: .relaxed))
    }
    return result
  }
}
