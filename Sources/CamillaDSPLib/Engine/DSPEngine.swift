// Public actor exposed to CamillaDSP-Monitor.
//
// The Monitor app was originally written against the UniFFI-generated
// bindings of the Rust CamillaDSP library; this actor preserves that exact
// API surface (`start(configJson:)`, `getSpectrum`, `getVuLevels`, etc.) so
// the Monitor sources compile unchanged. Internally it drives a
// `DSPEngineCore` plus a `SpectrumAnalyzer` and bridges between the two
// vocabularies.

import Foundation
import Logging
import Synchronization

// MARK: - Public types (FFI-shaped)

public struct VuLevels: Sendable {
  public let playback_rms: [Float]
  public let playback_peak: [Float]
  public let capture_rms: [Float]
  public let capture_peak: [Float]
}

/// Engine processing state. Used both internally by `DSPEngineCore`
/// (stored as `Atomic<UInt8>` via `rawByte`) and by the public actor's
/// `getStatus()` API, so it lives in one place.
///
/// `.paused` is set automatically by the silence-detection counter when
/// the capture signal stays below `silenceThreshold` for more than
/// `silenceTimeout` seconds (matching upstream CamillaDSP).
/// `.stalled` is set when the capture device hasn't produced fresh
/// samples for the watchdog timeout — typically a HAL-level hang.
public enum ProcessingState: String, Codable, Sendable, Equatable {
  case inactive = "Inactive"
  case starting = "Starting"
  case running = "Running"
  case paused = "Paused"
  case stalled = "Stalled"

  /// Compact integer encoding for `Atomic<UInt8>` storage. Internal
  /// — only the round-trip via `init(rawByte:)` matters.
  var rawByte: UInt8 {
    switch self {
    case .inactive: return 0
    case .starting: return 1
    case .running: return 2
    case .paused: return 3
    case .stalled: return 4
    }
  }

  init(rawByte: UInt8) {
    switch rawByte {
    case 1: self = .starting
    case 2: self = .running
    case 3: self = .paused
    case 4: self = .stalled
    default: self = .inactive
    }
  }
}

/// Why the engine stopped. Single shared shape for engine internals
/// (`DSPEngineCore.stopReason`) and the public `StateUpdate` returned
/// by `DSPEngine.getStatus()`.
///
/// Some internal-only events (`.userRequest`, `.configChanged`) collapse
/// to `.none` because the Monitor app — and any other client — can't
/// usefully act on them; the audible result is identical to a fresh
/// start.
public enum ProcessingStopReason: Sendable, Equatable {
  case none
  case done
  case captureError(String)
  case playbackError(String)
  case captureFormatChange(Int)
  case playbackFormatChange(Int)
  case unknownError(String)
}

public struct StateUpdate: Sendable {
  public let state: ProcessingState
  public let stopReason: ProcessingStopReason
}

public struct Spectrum: Sendable {
  public let frequencies: [Float]
  public let magnitudes: [Float]
}

public struct AudioSamples: Sendable {
  public let channels: [[Float]]

  public init(channels: [[Float]]) {
    self.channels = channels
  }

  public var left: [Float] { channels.first ?? [] }
  public var right: [Float] { channels.count > 1 ? channels[1] : (channels.first ?? []) }
}

public struct AudioDevice: Identifiable, Sendable {
  public var id: String { name }
  public let name: String
  public init(name: String) { self.name = name }
}

public enum LogLevel: String, CaseIterable, Identifiable, Sendable {
  case off = "Off"
  case error = "Error"
  case warn = "Warn"
  case info = "Info"
  case debug = "Debug"
  case trace = "Trace"
  public var id: String { rawValue }

  /// Compact byte encoding for `Atomic<UInt8>` storage in
  /// `MutableLogLevel`. The exact mapping is internal.
  var rawByte: UInt8 {
    switch self {
    case .off: return 0
    case .error: return 1
    case .warn: return 2
    case .info: return 3
    case .debug: return 4
    case .trace: return 5
    }
  }

  init(rawByte: UInt8) {
    switch rawByte {
    case 0: self = .off
    case 1: self = .error
    case 2: self = .warn
    case 4: self = .debug
    case 5: self = .trace
    default: self = .info
    }
  }
}

/// Errors thrown across the actor's public API. Mirrors the cases the
/// Monitor's old FFI-derived `AudioBackendError` enum exposed.
public enum AudioBackendError: Error, LocalizedError, Sendable {
  case configParse(message: String)
  case commandSend(message: String)
  case invalidSamplerate(message: String)
  case spectrumCompute(message: String)

  public var errorDescription: String? {
    switch self {
    case .configParse(let m): return "Config parse error: \(m)"
    case .commandSend(let m): return "Command send error: \(m)"
    case .invalidSamplerate(let m): return "Invalid samplerate: \(m)"
    case .spectrumCompute(let m): return "Spectrum compute error: \(m)"
    }
  }
}

// MARK: - The actor

public actor DSPEngine {

  /// The underlying engine. Recreated on every `start(configJson:)` call
  /// because `DSPEngineCore` is single-shot (its threads bind to one
  /// config). `nil` between calls.
  private var core: DSPEngineCore?

  /// Always-on spectrum analyzer.
  private let spectrum = SpectrumAnalyzer()

  /// Audio history buffers for spectrum and samples retrieval.
  private let captureBuffer = AudioHistoryBuffer()
  private let playbackBuffer = AudioHistoryBuffer()

  /// Cached desired volume / mute, applied to every newly started engine.
  /// `setVolume` and `setMute` are called by Monitor before `start`, and
  /// the values must persist across engine restarts.
  private var desiredVolumeDb: PrcFmt = 0.0
  private var desiredMute: Bool = false

  /// Last known stop reason, surfaced via `getStatus()`. Cleared on each
  /// successful `start`.
  private var lastStopReason: ProcessingStopReason?

  public init() {
    // Logging.bootstrap is a process-global hook; the Monitor app sets
    // its own handler so we deliberately don't touch it here.
  }

  // MARK: Lifecycle

  public func start(configJson: String) async throws {
    // Parse first; if the JSON is invalid surface a configParse error.
    let parsed: CamillaDSPConfig
    do {
      parsed = try ConfigLoader.parse(json: configJson)
    } catch {
      if let existing = core {
        existing.stop(reason: .none)
        core = nil
      }
      throw AudioBackendError.configParse(message: "\(error)")
    }

    // Hot-path: if the engine is already running and only filters /
    // mixers / pipeline changed, rebuild the pipeline in place
    // without touching the CoreAudio units. This is what users hit
    // when they tweak EQ bands or toggle a crossfeed stage —
    // restarting the audio device every time would cause an audible
    // glitch and reset the spectrum analyser's history.
    if let existing = core,
      existing.state != .inactive,
      existing.currentConfig.devices == parsed.devices
    {
      do {
        try existing.reloadConfig(parsed)
      } catch {
        existing.stop(reason: .none)
        core = nil
        throw AudioBackendError.configParse(message: "\(error)")
      }
      return
    }

    // Otherwise tear the engine down — either it wasn't running, or
    // the device section changed (sample rate, chunk size, named
    // device, channel count, format, resampler config, …). A
    // configuration restart is a clean stop from the Monitor's
    // perspective, so we surface `.none` rather than synthesise a
    // dedicated case.
    if let previous = core, previous.state != .inactive {
      previous.stop(reason: .none)
    }
    // If the captureLoop's HAL-rate-change path triggered the stop,
    // our `previous.stop(.none)` above is a no-op (idempotent
    // guard). The captureLoop's stop is still finishing on its
    // own thread — wait for it to land in `.inactive` before we
    // open new HAL units. Otherwise the old close's hog-mode
    // release races the new open's acquire, and on the same
    // physical device the new playback ends up un-hogged.
    if let previous = core {
      var waited: TimeInterval = 0
      while previous.state != .inactive, waited < 1.0 {
        try? await Task.sleep(nanoseconds: 5_000_000)
        waited += 0.005
      }
    }
    core = nil

    // Spin up a fresh engine.
    let engine = DSPEngineCore(config: parsed)

    // Plumb the desired volume/mute into the engine's processing
    // parameters BEFORE start. `Pipeline.init` reads
    // `processingParams.currentVolume` to seed the implicit master
    // volume filter — if we set this after start, the filter would
    // briefly run at 0 dB and then ramp, causing an audible click.
    engine.processingParams.targetVolume = desiredVolumeDb
    engine.processingParams.currentVolume = desiredVolumeDb
    engine.processingParams.isMuted = desiredMute

    // Hook up spectrum / sample taps. The closures run on the
    // processing thread; they must be quick — `SpectrumAnalyzer`
    // does only a memcpy under a small lock per channel.
    captureBuffer.reset(channels: parsed.devices.capture.channels)
    playbackBuffer.reset(channels: parsed.devices.playback.channels)

    let capBuf = self.captureBuffer
    let pbBuf = self.playbackBuffer
    engine.onChunkCaptured = { chunk in capBuf.append(chunk: chunk) }
    engine.onChunkProcessed = { chunk in pbBuf.append(chunk: chunk) }

    do {
      try engine.start()
    } catch {
      throw AudioBackendError.commandSend(message: "\(error)")
    }
    self.core = engine
    self.lastStopReason = nil
  }

  public func stop() {
    if let engine = core, engine.state != .inactive {
      engine.stop(reason: .none)
      lastStopReason = ProcessingStopReason.none
    }
    core = nil
  }

  public func setVolume(_ db: Float) {
    desiredVolumeDb = PrcFmt(db)
    core?.processingParams.targetVolume = PrcFmt(db)
  }

  public func setMute(_ mute: Bool) {
    desiredMute = mute
    core?.processingParams.isMuted = mute
  }

  // MARK: Direct fetch APIs

  public func getStatus() -> StateUpdate {
    let state: ProcessingState
    let reason: ProcessingStopReason
    if let core {
      state = core.state
      reason = core.stopReason ?? lastStopReason ?? .none
    } else {
      state = .inactive
      reason = lastStopReason ?? .none
    }
    return StateUpdate(state: state, stopReason: reason)
  }

  public func getVuLevels() -> VuLevels {
    guard let core else {
      return VuLevels(
        playback_rms: [], playback_peak: [],
        capture_rms: [], capture_peak: [])
    }
    let p = core.processingParams
    return VuLevels(
      playback_rms: p.playbackSignalRms.map { Float($0) },
      playback_peak: p.playbackSignalPeak.map { Float($0) },
      capture_rms: p.captureSignalRms.map { Float($0) },
      capture_peak: p.captureSignalPeak.map { Float($0) }
    )
  }

  public func getSpectrum(
    isCapture: Bool,
    channel: UInt32?,
    minFreq: Double,
    maxFreq: Double,
    nBins: UInt32
  ) throws -> Spectrum {
    guard let core else {
      throw AudioBackendError.spectrumCompute(message: "Engine not running")
    }
    let samplerate = core.currentConfig.devices.samplerate
    do {
      let result = try spectrum.compute(
        buffer: isCapture ? captureBuffer : playbackBuffer,
        channel: channel.map { Int($0) },
        minFreq: minFreq,
        maxFreq: maxFreq,
        nBins: Int(nBins),
        samplerate: samplerate
      )
      return Spectrum(frequencies: result.frequencies, magnitudes: result.magnitudes)
    } catch {
      throw AudioBackendError.spectrumCompute(message: "\(error)")
    }
  }

  public func getSamples(isCapture: Bool, nFrames: UInt32) throws -> AudioSamples {
    guard core != nil else {
      throw AudioBackendError.spectrumCompute(message: "Engine not running")
    }
    let buffer = isCapture ? captureBuffer : playbackBuffer
    guard buffer.hasData else { throw SpectrumError.bufferEmpty }
    let n = Swift.max(0, Swift.min(Int(nFrames), kRingBufferCapacity))
    let channelCount = buffer.channels

    var result: [[Float]] = []
    for ch in 0..<channelCount {
      var chData = [Float](repeating: 0, count: n)
      _ = try buffer.readLatest(into: &chData, count: n, channel: ch)
      result.append(chData)
    }

    return AudioSamples(channels: result)
  }

  // MARK: Device discovery

  public func getAvailableDevices(backend: String, input: Bool) -> [AudioDevice] {
    // Monitor only ever passes "coreaudio" / "CoreAudio". Other backends
    // would need the corresponding HAL plumbing — the Swift port doesn't
    // ship with ALSA/Pipewire/WASAPI, so silently return empty for them.
    guard backend.lowercased() == "coreaudio" else { return [] }
    let raw =
      input
      ? CoreAudioCapture.listDevices()
      : CoreAudioPlayback.listDevices()
    return raw.map { AudioDevice(name: $0.name) }
  }

  public func getDeviceCapabilities(
    backend: String,
    device: String,
    isCapture: Bool
  ) -> AudioDeviceDescriptor? {
    guard backend.lowercased() == "coreaudio" else { return nil }
    return CoreAudioCapabilities.describe(deviceName: device, isCapture: isCapture)
  }

  // MARK: Logging

  public func setLogLevel(_ level: LogLevel) {
    // Record on the process-wide singleton. A swift-log handler
    // installed by the host app can consult `MutableLogLevel.current`
    // to decide whether to drop a record. `LoggingSystem.bootstrap`
    // is a one-shot precondition, so we don't install a handler
    // here ourselves — the Monitor app owns its own logging setup.
    MutableLogLevel.current = level
  }
}

/// Process-wide knob that any swift-log handler can consult to honour
/// `DSPEngine.setLogLevel(...)`. Defaults to `.info`. Backed by an
/// `Atomic<UInt8>` — wait-free, naturally `Sendable`, no lock, no
/// `nonisolated(unsafe)`. Stores `LogLevel` directly via its
/// `rawByte` encoding; no swift-log `Logger.Level` round-trip is
/// needed.
public enum MutableLogLevel {

  private static let storage = Atomic<UInt8>(LogLevel.info.rawByte)

  public static var current: LogLevel {
    get { LogLevel(rawByte: storage.load(ordering: .acquiring)) }
    set { storage.store(newValue.rawByte, ordering: .releasing) }
  }
}
