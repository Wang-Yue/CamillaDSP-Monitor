import DSPConfig
import Foundation

extension AudioBackendError {
  init(_ dspError: DspError) {
    switch dspError {
    case .ConfigParseError(let message): self = .configParse(message: message)
    case .CommandSendError(let message): self = .commandSend(message: message)
    case .InvalidSamplerate(let message): self = .invalidSamplerate(message: message)
    case .SpectrumComputeError(let message): self = .spectrumCompute(message: message)
    }
  }
}

extension LogLevel {
  public var dspLogLevel: DspLogLevel {
    switch self {
    case .off: return .off
    case .error: return .error
    case .warn: return .warn
    case .info: return .info
    case .debug: return .debug
    case .trace: return .trace
    }
  }
}

extension ProcessingState {
  init(_ dspState: DspState) {
    switch dspState {
    case .running: self = .running
    case .paused: self = .paused
    case .inactive: self = .inactive
    case .starting: self = .starting
    case .stalled: self = .stalled
    }
  }
}

extension ProcessingStopReason {
  init(_ dspStopReason: DspStopReason) {
    switch dspStopReason {
    case .none: self = .none
    case .done: self = .done
    case .captureError(let message): self = .captureError(message)
    case .playbackError(let message): self = .playbackError(message)
    case .captureFormatChange(let rate): self = .captureFormatChange(Int(rate))
    case .playbackFormatChange(let rate): self = .playbackFormatChange(Int(rate))
    case .unknownError(let message): self = .unknownError(message)
    }
  }
}

extension AudioSamples {
  init(left: [Float], right: [Float]) {
    self.init(channels: [left, right])
  }
}

public actor DSPEngine {
  let engine: CamillaEngine = CamillaEngine()

  public init() {
    print("[DSPEngine] Initializing CamillaDSP library engine...")
  }

  // MARK: - Commands

  public static let isSwiftEngine = false

  public func start(configJson: String) async throws {
    do {
      try engine.setConfig(json: configJson)
    } catch let error as DspError {
      throw AudioBackendError(error)
    }
  }

  public func stop() async {
    engine.stop()
  }

  public func setVolume(_ db: Float) async {
    engine.setVolume(volume: db)
  }

  public func setMute(_ mute: Bool) async {
    engine.setMute(mute: mute)
  }

  public func getAvailableDevices(backend: String, input: Bool) async -> [AudioDevice] {
    let devices = engine.getAvailableDevices(backend: backend, input: input)
    return devices.map { AudioDevice(name: $0) }
  }

  public func getDeviceCapabilities(
    backend: String, device: String, isCapture: Bool
  ) async -> AudioDeviceDescriptor? {
    let json = engine.getDeviceCapabilities(backend: backend, device: device, input: isCapture)
    guard let data = json.data(using: .utf8) else { return nil }
    do {
      return try JSONDecoder().decode(AudioDeviceDescriptor.self, from: data)
    } catch {
      print("[DSPEngine] Failed to decode device capabilities: \(error)")
      return nil
    }
  }

  // MARK: - Direct Fetch APIs

  public func getVuLevels() async -> VuLevels {
    let levels = engine.getVuLevels()
    return VuLevels(
      playback_rms: levels.playbackRms,
      playback_peak: levels.playbackPeak,
      capture_rms: levels.captureRms,
      capture_peak: levels.capturePeak
    )
  }

  public func getStatus() async -> StateUpdate {
    let status = engine.getStatus()
    return StateUpdate(
      state: ProcessingState(status.state),
      stopReason: ProcessingStopReason(status.stopReason)
    )
  }

  public func getSpectrum(
    isCapture: Bool, channel: UInt32?, minFreq: Double, maxFreq: Double, nBins: UInt32
  ) async throws -> Spectrum {
    do {
      let data = try engine.getSpectrum(
        input: isCapture, channel: channel, minFreq: minFreq, maxFreq: maxFreq, nBins: nBins)
      return Spectrum(frequencies: data.frequencies, magnitudes: data.magnitudes)
    } catch let error as DspError {
      throw AudioBackendError(error)
    }
  }

  public func getSamples(isCapture: Bool, nFrames: UInt32) async throws -> AudioSamples {
    do {
      let data = try engine.getSamples(input: isCapture, nFrames: nFrames)
      return AudioSamples(left: data.left, right: data.right)
    } catch let error as DspError {
      throw AudioBackendError(error)
    }
  }

  public func setLogLevel(_ level: LogLevel) async {
    engine.setLogLevel(level: level.dspLogLevel)
  }
}
