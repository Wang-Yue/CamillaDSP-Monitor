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
    var query = cdsp_vu_levels_t()
    guard cdsp_get_vu_levels(e, &query) else {
      return VuLevels(playback_rms: [], playback_peak: [], capture_rms: [], capture_peak: [])
    }
    let pbCh = Int(query.playback_channels)
    let capCh = Int(query.capture_channels)
    var pbRms = [Float](repeating: 0, count: pbCh)
    var pbPeak = [Float](repeating: 0, count: pbCh)
    var capRms = [Float](repeating: 0, count: capCh)
    var capPeak = [Float](repeating: 0, count: capCh)

    let ok: Bool = pbRms.withUnsafeMutableBufferPointer { pbRmsPtr in
      pbPeak.withUnsafeMutableBufferPointer { pbPeakPtr in
        capRms.withUnsafeMutableBufferPointer { capRmsPtr in
          capPeak.withUnsafeMutableBufferPointer { capPeakPtr in
            var levels = cdsp_vu_levels_t(
              playback_rms: pbRmsPtr.baseAddress,
              playback_peak: pbPeakPtr.baseAddress,
              capture_rms: capRmsPtr.baseAddress,
              capture_peak: capPeakPtr.baseAddress,
              playback_channels: 0,
              capture_channels: 0
            )
            return cdsp_get_vu_levels(e, &levels)
          }
        }
      }
    }
    guard ok else {
      return VuLevels(playback_rms: [], playback_peak: [], capture_rms: [], capture_peak: [])
    }
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
    let n = Int(nBins)
    guard n > 0 else {
      throw AudioBackendError.bufferEmpty
    }
    let side = isCapture ? CDSP_SPECTRUM_SIDE_CAPTURE : CDSP_SPECTRUM_SIDE_PLAYBACK
    var freqs = [Float](repeating: 0, count: n)
    var mags = [Float](repeating: 0, count: n)
    var countPopulated: size_t = 0

    let success: Bool = freqs.withUnsafeMutableBufferPointer { fPtr in
      mags.withUnsafeMutableBufferPointer { mPtr in
        var res = cdsp_spectrum_t(
          frequencies: fPtr.baseAddress,
          magnitudes: mPtr.baseAddress,
          count: 0
        )
        let ok: Bool
        if let ch = channel {
          var chSizeT = size_t(ch)
          ok = cdsp_get_spectrum(e, side, &chSizeT, Float(minFreq), Float(maxFreq), n, &res)
        } else {
          ok = cdsp_get_spectrum(e, side, nil, Float(minFreq), Float(maxFreq), n, &res)
        }
        countPopulated = res.count
        return ok
      }
    }
    if !success || countPopulated == 0 {
      throw AudioBackendError.bufferEmpty
    }
    if countPopulated < n {
      freqs.removeLast(n - countPopulated)
      mags.removeLast(n - countPopulated)
    }
    return Spectrum(frequencies: freqs, magnitudes: mags)
  }

  public func getSamples(isCapture: Bool, nFrames: UInt32) async throws -> AudioSamples {
    guard let e = engine else { return AudioSamples(channels: [[], []]) }
    var err = cdsp_backend_error_t()
    let framesCount = Int(nFrames)
    guard framesCount > 0 else { return AudioSamples(channels: [[], []]) }

    var query = cdsp_audio_samples_t()
    guard cdsp_get_samples(e, isCapture, framesCount, &query, &err) else {
      if err.type == CDSP_BACKEND_ERR_UNKNOWN {
        throw AudioBackendError.engineNotRunning
      } else {
        throw AudioBackendError.bufferEmpty
      }
    }

    let chCount = Int(query.channels_count)
    guard chCount > 0 else { return AudioSamples(channels: []) }

    let allocatedPtrs = (0..<chCount).map { _ in UnsafeMutablePointer<Float>.allocate(capacity: framesCount) }
    defer {
      for ptr in allocatedPtrs {
        ptr.deallocate()
      }
    }

    var ptrsCopy: [UnsafeMutablePointer<Float>?] = allocatedPtrs.map { Optional($0) }
    let actualFrames: Int = try ptrsCopy.withUnsafeMutableBufferPointer { ptrsBuf in
      var samples = cdsp_audio_samples_t(
        channels: ptrsBuf.baseAddress,
        channels_count: 0,
        frames: 0
      )
      guard cdsp_get_samples(e, isCapture, framesCount, &samples, &err) else {
        if err.type == CDSP_BACKEND_ERR_UNKNOWN {
          throw AudioBackendError.engineNotRunning
        } else {
          throw AudioBackendError.bufferEmpty
        }
      }
      return Int(samples.frames)
    }

    var channels: [[Float]] = []
    channels.reserveCapacity(chCount)
    for ptr in allocatedPtrs {
      channels.append(Array(UnsafeBufferPointer(start: ptr, count: actualFrames)))
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
      let id = withUnsafePointer(to: dPtr[i].identifier) { ptr in
        ptr.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
      }
      let name = withUnsafePointer(to: dPtr[i].name) { ptr in
        ptr.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
      }
      res.append(AudioDevice(id: id, name: name))
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
