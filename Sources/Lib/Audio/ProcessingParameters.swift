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

  public init(captureChannels: Int = 2, playbackChannels: Int = 2) {
    _ = captureChannels
    _ = playbackChannels
    self._targetVolume = AtomicDouble(Self.defaultVolume)
    self._currentVolume = AtomicDouble(Self.defaultVolume)
    self._muted = Atomic<Bool>(Self.defaultMute)
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

  // MARK: - Chunk-based peak update (no-allocation, audio-thread safe)

  public func updateCaptureLevels(from chunk: AudioChunk) -> PrcFmt {
    let channelCount = chunk.channels
    guard channelCount > 0 else { return -1000.0 }
    let frameCount = chunk.validFrames
    var maxPeak: PrcFmt = -1000.0
    for i in 0..<channelCount {
      let buffer = UnsafeBufferPointer(chunk[i])
      let peakDb = PrcFmt.toDB(DSPOps.peakAbsolute(buffer, count: frameCount))
      if peakDb > maxPeak {
        maxPeak = peakDb
      }
    }
    return maxPeak
  }
}
