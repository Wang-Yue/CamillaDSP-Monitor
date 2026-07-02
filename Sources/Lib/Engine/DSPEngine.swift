import DSPAudio
import DSPBackend
import DSPConfig
import DSPLogging
import DSPPipeline
import Foundation
import Synchronization

public actor SwiftDSPEngine {
  private let logger = Logger(label: "dsp.engine")
  private var core: DSPEngineCore?
  private let spectrum = SpectrumAnalyzer()
  private let captureBuffer = AudioHistoryBuffer()
  private let playbackBuffer = AudioHistoryBuffer()
  private var desiredVolumeDb: PrcFmt = 0.0
  private var desiredMute: Bool = false
  private var lastStopReason: ProcessingStopReason?

  public init() {}

  public func setConfig(json: String) async throws {
    logger.info("Set config: %s", .string(json))
    let parsed: DSPConfiguration
    do {
      parsed = try ConfigLoader.parse(json: json)
    } catch {
      if let existing = core {
        existing.stop(reason: .none)
        core = nil
      }
      throw AudioBackendError.configParse(message: "\(error)")
    }

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

    if let previous = core, previous.state != .inactive {
      previous.stop(reason: .none)
    }

    if let previous = core {
      var waited: TimeInterval = 0
      while previous.state != .inactive, waited < 1.0 {
        try? await Task.sleep(nanoseconds: 5_000_000)
        waited += 0.005
      }
    }
    core = nil

    let engine = DSPEngineCore(config: parsed)
    engine.processingParams.targetVolume = desiredVolumeDb
    engine.processingParams.currentVolume = desiredVolumeDb
    engine.processingParams.isMuted = desiredMute

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
      throw AudioBackendError.engineNotRunning
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
      throw AudioBackendError.engineNotRunning
    }
    let buffer = isCapture ? captureBuffer : playbackBuffer
    guard buffer.hasData else { throw AudioBackendError.bufferEmpty }
    let n = Swift.max(0, Swift.min(Int(nFrames), kRingBufferCapacity))
    let channelCount = buffer.channels

    var result: [[Float]] = []
    for ch in 0..<channelCount {
      var chData = [Float](repeating: 0, count: n)
      do {
        _ = try buffer.readLatest(into: &chData, count: n, channel: ch)
      } catch {
        throw AudioBackendError.bufferEmpty
      }
      result.append(chData)
    }

    return AudioSamples(channels: result)
  }
  public func setLogLevel(_ level: LogLevel) {
    MutableLogLevel.current = level
  }
  public func getAvailableDevices(backend: String, input: Bool) -> [AudioDevice] {
    guard backend.lowercased() == "coreaudio" else { return [] }
    let raw = input ? CoreAudioCapture.listDevices() : CoreAudioPlayback.listDevices()
    return raw.map { AudioDevice(name: $0.name) }
  }

  public func getDeviceCapabilities(backend: String, device: String, isCapture: Bool)
    -> AudioDeviceDescriptor?
  {
    guard backend.lowercased() == "coreaudio" else { return nil }
    return CoreAudioCapabilities.describe(deviceName: device, isCapture: isCapture)
  }

  public func getActiveConfig() -> DSPConfiguration? {
    return core?.currentConfig
  }

  public func getProcessingParameters() -> ProcessingParameters? {
    return core?.processingParams
  }
}
