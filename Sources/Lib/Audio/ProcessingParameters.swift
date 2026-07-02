// Concurrency model
// -----------------
// Every field is backed by an `Atomic` from the standard `Synchronization`
// module — no `NSLock`, no `@unchecked Sendable`.
// Target volume, current volume, and mute states are kept for 5 faders (Main, Aux 1-4)
// as separate inline atomic variables to avoid heap allocation and conform to non-copyable requirements.

import Synchronization

public enum Fader: Int, Sendable {
  case main = 0
  case aux1 = 1
  case aux2 = 2
  case aux3 = 3
  case aux4 = 4
}

extension Fader: Codable {
  public init(from decoder: Decoder) throws {
    let container = try decoder.singleValueContainer()
    if let intValue = try? container.decode(Int.self) {
      if let fader = Fader(rawValue: intValue) {
        self = fader
        return
      }
    }
    let stringValue = try container.decode(String.self)
    switch stringValue.lowercased() {
    case "main": self = .main
    case "aux1": self = .aux1
    case "aux2": self = .aux2
    case "aux3": self = .aux3
    case "aux4": self = .aux4
    default:
      throw DecodingError.dataCorruptedError(
        in: container,
        debugDescription: "Cannot decode Fader from \(stringValue)"
      )
    }
  }

  public func encode(to encoder: Encoder) throws {
    var container = encoder.singleValueContainer()
    switch self {
    case .main: try container.encode("Main")
    case .aux1: try container.encode("Aux1")
    case .aux2: try container.encode("Aux2")
    case .aux3: try container.encode("Aux3")
    case .aux4: try container.encode("Aux4")
    }
  }
}

public final class ProcessingParameters: Sendable {

  /// Default volume (dB) when an engine starts.
  public static let defaultVolume: PrcFmt = 0.0
  /// Default mute state.
  public static let defaultMute = false

  // MARK: - Storage

  /// Target volume (dB) for fader 0 (Main) — what the user has asked for. UI thread writes;
  /// VolumeFilter reads on every chunk.
  private let _targetVolume0: AtomicDouble
  /// Target volume (dB) for fader 1 (Aux 1).
  private let _targetVolume1: AtomicDouble
  /// Target volume (dB) for fader 2 (Aux 2).
  private let _targetVolume2: AtomicDouble
  /// Target volume (dB) for fader 3 (Aux 3).
  private let _targetVolume3: AtomicDouble
  /// Target volume (dB) for fader 4 (Aux 4).
  private let _targetVolume4: AtomicDouble

  /// Current volume (dB) for fader 0 (Main) — tracking ramp progress.
  private let _currentVolume0: AtomicDouble
  /// Current volume (dB) for fader 1 (Aux 1).
  private let _currentVolume1: AtomicDouble
  /// Current volume (dB) for fader 2 (Aux 2).
  private let _currentVolume2: AtomicDouble
  /// Current volume (dB) for fader 3 (Aux 3).
  private let _currentVolume3: AtomicDouble
  /// Current volume (dB) for fader 4 (Aux 4).
  private let _currentVolume4: AtomicDouble

  /// Mute state for fader 0 (Main). UI writes; VolumeFilter reads each chunk.
  private let _muted0: Atomic<Bool>
  /// Mute state for fader 1 (Aux 1).
  private let _muted1: Atomic<Bool>
  /// Mute state for fader 2 (Aux 2).
  private let _muted2: Atomic<Bool>
  /// Mute state for fader 3 (Aux 3).
  private let _muted3: Atomic<Bool>
  /// Mute state for fader 4 (Aux 4).
  private let _muted4: Atomic<Bool>

  /// Per-channel signal levels (dB).
  private let _captureSignalPeak: AtomicLevels
  private let _captureSignalRms: AtomicLevels
  private let _playbackSignalPeak: AtomicLevels
  private let _playbackSignalRms: AtomicLevels

  public init(captureChannels: Int, playbackChannels: Int) {
    self._targetVolume0 = AtomicDouble(Self.defaultVolume)
    self._targetVolume1 = AtomicDouble(Self.defaultVolume)
    self._targetVolume2 = AtomicDouble(Self.defaultVolume)
    self._targetVolume3 = AtomicDouble(Self.defaultVolume)
    self._targetVolume4 = AtomicDouble(Self.defaultVolume)

    self._currentVolume0 = AtomicDouble(Self.defaultVolume)
    self._currentVolume1 = AtomicDouble(Self.defaultVolume)
    self._currentVolume2 = AtomicDouble(Self.defaultVolume)
    self._currentVolume3 = AtomicDouble(Self.defaultVolume)
    self._currentVolume4 = AtomicDouble(Self.defaultVolume)

    self._muted0 = Atomic<Bool>(Self.defaultMute)
    self._muted1 = Atomic<Bool>(Self.defaultMute)
    self._muted2 = Atomic<Bool>(Self.defaultMute)
    self._muted3 = Atomic<Bool>(Self.defaultMute)
    self._muted4 = Atomic<Bool>(Self.defaultMute)

    self._captureSignalPeak = AtomicLevels(channels: captureChannels)
    self._captureSignalRms = AtomicLevels(channels: captureChannels)
    self._playbackSignalPeak = AtomicLevels(channels: playbackChannels)
    self._playbackSignalRms = AtomicLevels(channels: playbackChannels)
  }

  // MARK: - Volume / Mute

  public func targetVolume(for fader: Fader) -> PrcFmt {
    switch fader {
    case .main: return _targetVolume0.value
    case .aux1: return _targetVolume1.value
    case .aux2: return _targetVolume2.value
    case .aux3: return _targetVolume3.value
    case .aux4: return _targetVolume4.value
    }
  }

  public func setTargetVolume(_ value: PrcFmt, for fader: Fader) {
    switch fader {
    case .main: _targetVolume0.value = value
    case .aux1: _targetVolume1.value = value
    case .aux2: _targetVolume2.value = value
    case .aux3: _targetVolume3.value = value
    case .aux4: _targetVolume4.value = value
    }
  }

  public func currentVolume(for fader: Fader) -> PrcFmt {
    switch fader {
    case .main: return _currentVolume0.value
    case .aux1: return _currentVolume1.value
    case .aux2: return _currentVolume2.value
    case .aux3: return _currentVolume3.value
    case .aux4: return _currentVolume4.value
    }
  }

  public func setCurrentVolume(_ value: PrcFmt, for fader: Fader) {
    switch fader {
    case .main: _currentVolume0.value = value
    case .aux1: _currentVolume1.value = value
    case .aux2: _currentVolume2.value = value
    case .aux3: _currentVolume3.value = value
    case .aux4: _currentVolume4.value = value
    }
  }

  public func isMuted(for fader: Fader) -> Bool {
    switch fader {
    case .main: return _muted0.load(ordering: .acquiring)
    case .aux1: return _muted1.load(ordering: .acquiring)
    case .aux2: return _muted2.load(ordering: .acquiring)
    case .aux3: return _muted3.load(ordering: .acquiring)
    case .aux4: return _muted4.load(ordering: .acquiring)
    }
  }

  public func setMuted(_ value: Bool, for fader: Fader) {
    switch fader {
    case .main: _muted0.store(value, ordering: .releasing)
    case .aux1: _muted1.store(value, ordering: .releasing)
    case .aux2: _muted2.store(value, ordering: .releasing)
    case .aux3: _muted3.store(value, ordering: .releasing)
    case .aux4: _muted4.store(value, ordering: .releasing)
    }
  }

  public var targetVolume: PrcFmt {
    get { targetVolume(for: .main) }
    set { setTargetVolume(newValue, for: .main) }
  }

  public var currentVolume: PrcFmt {
    get { currentVolume(for: .main) }
    set { setCurrentVolume(newValue, for: .main) }
  }

  public var isMuted: Bool {
    get { isMuted(for: .main) }
    set { setMuted(newValue, for: .main) }
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

  /// Asynchronously update the capture-side peak and RMS levels on the audio thread.
  /// Does not allocate.
  public func updateCaptureLevels(from chunk: AudioChunk) -> PrcFmt {
    return updateLevels(from: chunk, peakStorage: _captureSignalPeak, rmsStorage: _captureSignalRms)
  }

  /// Asynchronously update the playback-side peak and RMS levels on the audio thread.
  /// Does not allocate.
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
