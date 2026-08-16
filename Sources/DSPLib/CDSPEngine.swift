// Public actor exposed to DSPMonitor.
//
// Bridges the C library engine (CLib/Engine) to the Swift API surface
// expected by DSPMonitor and DSPCLI.

import CDSP
import DSPConfig
import Foundation

// MARK: - The actor

extension Fader {
  var cValue: cdsp_fader_t {
    switch self {
    case .main: return CDSP_FADER_MAIN
    case .aux1: return CDSP_FADER_AUX1
    case .aux2: return CDSP_FADER_AUX2
    case .aux3: return CDSP_FADER_AUX3
    case .aux4: return CDSP_FADER_AUX4
    }
  }
}

public actor DSPEngine {
  public static let isCEngine = true
  public static let isRustEngine = false
  nonisolated(unsafe) private let engine: OpaquePointer?

  public init() {
    DSPEngine.dispatchLog(level: "INFO", label: "DSPEngine", message: "Initializing C library engine...")
    self.engine = cdsp_engine_create()
  }

  deinit {
    if let e = engine {
      cdsp_engine_free(e)
    }
  }

  // MARK: Lifecycle

  public func start(configJson: String) async throws {
    guard let e = engine else { return }
    var err = cdsp_backend_error_t()
    let success = configJson.withCString { cStr in
      cdsp_set_config_json(e, cStr, &err)
    }
    if !success {
      let msg = withUnsafePointer(to: &err.message) { ptr in
        ptr.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
      }
      if err.type == CDSP_BACKEND_ERR_CONFIG_PARSE {
        throw AudioBackendError.configParse(message: msg)
      } else {
        throw AudioBackendError.commandSend(message: msg)
      }
    }
  }

  public func setConfig(json: String) async throws {
    try await start(configJson: json)
  }

  public func stop() async {
    guard let e = engine else { return }
    cdsp_stop(e)
  }

  public func setFaderVolume(_ fader: Fader, _ db: Float) async {
    guard let e = engine else { return }
    cdsp_set_fader_volume(e, fader.cValue, db, false)
  }

  public func setFaderMute(_ fader: Fader, _ mute: Bool) async {
    guard let e = engine else { return }
    cdsp_set_fader_mute(e, fader.cValue, mute)
  }

  public func getFaderVolume(_ fader: Fader) async -> Float {
    guard let e = engine else { return 0.0 }
    return cdsp_get_fader_volume(e, fader.cValue)
  }

  public func isFaderMuted(_ fader: Fader) async -> Bool {
    guard let e = engine else { return false }
    return cdsp_get_fader_mute(e, fader.cValue)
  }

  public func getStatus() async -> StateUpdate {
    guard let e = engine else { return StateUpdate(state: .inactive, stopReason: .none) }
    let st = cdsp_get_state(e)
    let state: ProcessingState
    switch st {
    case CDSP_PROCESSING_STATE_RUNNING: state = .running
    case CDSP_PROCESSING_STATE_PAUSED: state = .paused
    case CDSP_PROCESSING_STATE_INACTIVE: state = .inactive
    case CDSP_PROCESSING_STATE_STARTING: state = .starting
    case CDSP_PROCESSING_STATE_STALLED: state = .stalled
    default: state = .inactive
    }
    var rawStopReason = cdsp_stop_reason_t()
    cdsp_get_stop_reason(e, &rawStopReason)
    let stopReason: ProcessingStopReason
    switch rawStopReason.type {
    case CDSP_STOP_REASON_NONE: stopReason = .none
    case CDSP_STOP_REASON_DONE: stopReason = .done
    case CDSP_STOP_REASON_CAPTURE_ERROR:
       let msg = withUnsafePointer(to: rawStopReason.message) { ptr in
         ptr.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
       }
       stopReason = .captureError(msg)
    case CDSP_STOP_REASON_PLAYBACK_ERROR:
       let msg = withUnsafePointer(to: rawStopReason.message) { ptr in
         ptr.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
       }
       stopReason = .playbackError(msg)
    case CDSP_STOP_REASON_CAPTURE_FORMAT_CHANGE:
       stopReason = .captureFormatChange(Int(rawStopReason.format_change_rate))
    case CDSP_STOP_REASON_PLAYBACK_FORMAT_CHANGE:
       stopReason = .playbackFormatChange(Int(rawStopReason.format_change_rate))
    case CDSP_STOP_REASON_UNKNOWN_ERROR:
       let msg = withUnsafePointer(to: rawStopReason.message) { ptr in
         ptr.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
       }
       stopReason = .unknownError(msg)
    default: stopReason = .none
    }
    return StateUpdate(state: state, stopReason: stopReason)
  }

  public func getVuLevels() async -> VuLevels {
    guard let e = engine else {
      return VuLevels(playback_rms: [], playback_peak: [], capture_rms: [], capture_peak: [])
    }
    var levels = cdsp_vu_levels_t()
    guard cdsp_get_vu_levels(e, &levels) else {
      return VuLevels(playback_rms: [], playback_peak: [], capture_rms: [], capture_peak: [])
    }
    defer { cdsp_free_vu_levels(&levels) }
    let pbRms = Array(
      UnsafeBufferPointer(start: levels.playback_rms, count: Int(levels.playback_channels))
    ).map { Float($0) }
    let pbPeak = Array(
      UnsafeBufferPointer(start: levels.playback_peak, count: Int(levels.playback_channels))
    ).map { Float($0) }
    let capRms = Array(
      UnsafeBufferPointer(start: levels.capture_rms, count: Int(levels.capture_channels))
    ).map { Float($0) }
    let capPeak = Array(
      UnsafeBufferPointer(start: levels.capture_peak, count: Int(levels.capture_channels))
    ).map { Float($0) }
    return VuLevels(
      playback_rms: pbRms,
      playback_peak: pbPeak,
      capture_rms: capRms,
      capture_peak: capPeak
    )
  }

  public func getSpectrum(
    isCapture: Bool,
    channel: UInt32?,
    minFreq: Double,
    maxFreq: Double,
    nBins: UInt32
  ) async throws -> Spectrum {
    guard let e = engine else {
      throw AudioBackendError.engineNotRunning
    }
    var res = cdsp_spectrum_t()
    let side = isCapture ? CDSP_SPECTRUM_SIDE_CAPTURE : CDSP_SPECTRUM_SIDE_PLAYBACK
    let success: Bool
    if var ch = channel {
      success = cdsp_get_spectrum(e, side, &ch, minFreq, maxFreq, Int(nBins), &res)
    } else {
      success = cdsp_get_spectrum(e, side, nil, minFreq, maxFreq, Int(nBins), &res)
    }
    if !success {
      throw AudioBackendError.bufferEmpty
    }
    defer { cdsp_free_spectrum(&res) }
    let freqs = Array(UnsafeBufferPointer(start: res.frequencies, count: Int(res.count))).map { Float($0) }
    let mags = Array(UnsafeBufferPointer(start: res.magnitudes, count: Int(res.count))).map { Float($0) }
    return Spectrum(frequencies: freqs, magnitudes: mags)
  }

  public func getSamples(isCapture: Bool, nFrames: UInt32) async throws -> AudioSamples {
    guard let e = engine else { return AudioSamples(channels: [[], []]) }
    var err = cdsp_backend_error_t()
    guard let res = cdsp_get_samples(e, isCapture, Int(nFrames), &err) else {
      if err.type == CDSP_BACKEND_ERR_UNKNOWN {
        throw AudioBackendError.engineNotRunning
      } else {
        throw AudioBackendError.bufferEmpty
      }
    }
    defer { cdsp_free_samples(res) }
    var channels: [[Float]] = []
    for ch in 0..<Int(res.pointee.channels_count) {
      if let ptr = res.pointee.channels[ch] {
        let doubles = UnsafeBufferPointer(start: ptr, count: Int(res.pointee.frames))
        channels.append(doubles.map { Float($0) })
      } else {
        channels.append([])
      }
    }
    return AudioSamples(channels: channels)
  }

  public func getAvailableDevices(backend: String, input: Bool) async -> [AudioDevice] {
    guard engine != nil else { return [] }
    var devs: UnsafeMutablePointer<cdsp_device_info_t>? = nil
    var count = 0
    let success = backend.withCString { bStr in
      cdsp_get_available_devices(bStr, input, &devs, &count)
    }
    guard success, let dPtr = devs else { return [] }
    defer { free(dPtr) }
    guard count > 0 else { return [] }
    var res: [AudioDevice] = []
    for i in 0..<count {
      let name = withUnsafePointer(to: dPtr[i].name) { ptr in
        ptr.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
      }
      res.append(AudioDevice(name: name))
    }
    return res
  }

  public func getDeviceCapabilities(
    backend: String,
    device: String,
    isCapture: Bool
  ) async -> AudioDeviceDescriptor? {
    guard engine != nil else { return nil }
    var devErr = cdsp_device_error_t()
    var outDesc: UnsafeMutablePointer<cdsp_device_descriptor_t>? = nil
    let success = backend.withCString({ bStr in
      device.withCString { dStr in
        cdsp_get_device_capabilities(bStr, dStr, isCapture, &outDesc, &devErr)
      }
    })
    guard success, let desc = outDesc else {
      if let desc = outDesc {
        cdsp_free_device_capabilities(desc)
      }
      if devErr.type != CDSP_DEVICE_ERROR_NONE {
        let msg = withUnsafePointer(to: devErr.message) { ptr in
          ptr.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
        }
        DSPEngine.dispatchLog(level: "ERROR", label: "CDSPEngine", message: "Device capabilities error: \(msg)")
      }
      return nil
    }
    defer { cdsp_free_device_capabilities(desc) }
    let name = withUnsafePointer(to: desc.pointee.name) { ptr in
      ptr.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
    }
    var capSets: [DeviceCapabilitySet] = []
    for i in 0..<Int(desc.pointee.capability_sets_count) {
      let cSet = desc.pointee.capability_sets[i]
      var chCaps: [ChannelCapability] = []
      for j in 0..<Int(cSet.capabilities_count) {
        let chCap = cSet.capabilities[j]
        var srCaps: [SamplerateCapability] = []
        for k in 0..<Int(chCap.samplerates_count) {
          let srCap = chCap.samplerates[k]
          var fmts: [String] = []
          for m in 0..<Int(srCap.formats_count) {
            if let fStr = srCap.formats[m] {
              fmts.append(String(cString: fStr))
            }
          }
          srCaps.append(SamplerateCapability(samplerate: Int(srCap.samplerate), formats: fmts))
        }
        chCaps.append(ChannelCapability(channels: Int(chCap.channels), samplerates: srCaps))
      }
      let mode = withUnsafePointer(to: cSet.mode) { ptr in
        ptr.withMemoryRebound(to: CChar.self, capacity: 64) { String(cString: $0) }
      }
      capSets.append(DeviceCapabilitySet(mode: mode, capabilities: chCaps))
    }
    return AudioDeviceDescriptor(name: name, capability_sets: capSets)
  }

  public func setLogLevel(_ level: LogLevel) async {
    level.rawValue.withCString { cStr in
      cdsp_set_log_level(cStr)
    }
  }

  // MARK: - Log Callback Bridge

  public typealias LogCallback = @Sendable (_ level: String, _ label: String, _ message: String) -> Void
  nonisolated(unsafe) private static var logCallback: LogCallback?
  private static let logLock = NSLock()

  public static func dispatchLog(level: String, label: String, message: String) {
    let cb = logLock.withLock { logCallback }
    cb?(level, label, message)
  }

  public static func setLogCallback(_ callback: LogCallback?) {
    logLock.withLock {
      logCallback = callback
    }

    if callback != nil {
      cdsp_set_log_callback({ levelPtr, labelPtr, msgPtr, _ in
        guard let levelPtr, let labelPtr, let msgPtr else { return }
        let level = String(cString: levelPtr)
        let label = String(cString: labelPtr)
        let msg = String(cString: msgPtr)
        DSPEngine.dispatchLog(level: level, label: label, message: msg)
      }, nil)
    } else {
      cdsp_set_log_callback(nil, nil)
    }
  }
}
