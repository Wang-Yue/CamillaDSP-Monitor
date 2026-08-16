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

  public static let isRustEngine = true

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

  public func setFaderVolume(_ fader: Fader, _ db: Float) async {
    engine.setFaderVolume(fader: UInt32(fader.intValue), volume: db)
  }

  public func setFaderMute(_ fader: Fader, _ mute: Bool) async {
    engine.setFaderMute(fader: UInt32(fader.intValue), mute: mute)
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

  // MARK: - Log Callback Bridge (Pipes Interception for Rust Engine)

  public typealias LogCallback = @Sendable (_ level: String, _ label: String, _ message: String) -> Void

  nonisolated(unsafe) private static var logCallback: LogCallback?
  private static let logLock = NSLock()
  private static let outPipe = Pipe()
  private static let errPipe = Pipe()
  nonisolated(unsafe) private static var isPipeSetUp = false
  nonisolated(unsafe) private static var leftoverOutData = Data()
  nonisolated(unsafe) private static var leftoverErrData = Data()

  public static func setLogCallback(_ callback: LogCallback?) {
    logLock.lock()
    logCallback = callback
    logLock.unlock()

    if callback != nil {
      setupPipes()
    }
  }

  private static func setupPipes() {
    logLock.lock()
    guard !isPipeSetUp else {
      logLock.unlock()
      return
    }
    isPipeSetUp = true
    logLock.unlock()

    // Disable buffering for stdout and stderr
    setvbuf(stdout, nil, _IOLBF, 0)
    setvbuf(stderr, nil, _IOLBF, 0)

    let outHandle = outPipe.fileHandleForWriting
    let errHandle = errPipe.fileHandleForWriting

    dup2(outHandle.fileDescriptor, STDOUT_FILENO)
    dup2(errHandle.fileDescriptor, STDERR_FILENO)

    outPipe.fileHandleForReading.readabilityHandler = { handle in
      let data = handle.availableData
      guard !data.isEmpty else { return }
      processCapturedData(data, isErr: false)
    }

    errPipe.fileHandleForReading.readabilityHandler = { handle in
      let data = handle.availableData
      guard !data.isEmpty else { return }
      processCapturedData(data, isErr: true)
    }
  }

  private static func processCapturedData(_ data: Data, isErr: Bool) {
    logLock.lock()
    let cb = logCallback
    var leftover = isErr ? leftoverErrData : leftoverOutData
    leftover.append(data)

    guard let str = String(data: leftover, encoding: .utf8) else {
      if isErr { leftoverErrData = leftover } else { leftoverOutData = leftover }
      logLock.unlock()
      return
    }

    let lines = str.components(separatedBy: .newlines)
    if !str.hasSuffix("\n") {
      if isErr {
        leftoverErrData = lines.last?.data(using: .utf8) ?? Data()
      } else {
        leftoverOutData = lines.last?.data(using: .utf8) ?? Data()
      }
    } else {
      if isErr { leftoverErrData = Data() } else { leftoverOutData = Data() }
    }
    logLock.unlock()

    let completeLines = str.hasSuffix("\n") ? lines : lines.dropLast()
    for line in completeLines where !line.isEmpty {
      var level = isErr ? "ERROR" : "INFO"
      var label = ""
      var message = line

      let levels = ["ERROR", "WARN", "INFO", "DEBUG", "TRACE"]
      var foundLevel: String?
      var levelRange: Range<String.Index>?

      for lvl in levels {
        if let r = line.range(of: " \(lvl)  ") ?? line.range(of: " \(lvl) ") ?? line.range(of: "[\(lvl)]") ?? line.range(of: "\(lvl) ") {
          foundLevel = lvl
          levelRange = r
          break
        }
      }

      if let foundLevel, let levelRange {
        level = foundLevel
        let afterLevel = line[levelRange.upperBound...].trimmingCharacters(in: .whitespaces)

        if afterLevel.hasPrefix("[") {
          if let closeIdx = afterLevel.firstIndex(of: "]") {
            label = String(afterLevel[afterLevel.index(after: afterLevel.startIndex)..<closeIdx])
            let rest = afterLevel[afterLevel.index(after: closeIdx)...].trimmingCharacters(in: .whitespaces)
            if rest.hasPrefix(":") {
              message = String(rest.dropFirst()).trimmingCharacters(in: .whitespaces)
            } else {
              message = rest
            }
          } else {
            message = afterLevel
          }
        } else if let bracketIdx = afterLevel.firstIndex(of: "]") {
          label = String(afterLevel[..<bracketIdx]).trimmingCharacters(in: .whitespaces)
          var rest = String(afterLevel[afterLevel.index(after: bracketIdx)...]).trimmingCharacters(in: .whitespaces)
          if rest.hasPrefix(":") {
            rest = String(rest.dropFirst()).trimmingCharacters(in: .whitespaces)
          }
          message = rest
        } else if let colonIdx = afterLevel.firstIndex(of: ":") {
          let candidate = String(afterLevel[..<colonIdx]).trimmingCharacters(in: .whitespaces)
          if !candidate.contains(" ") {
            label = candidate
            message = String(afterLevel[afterLevel.index(after: colonIdx)...]).trimmingCharacters(in: .whitespaces)
          } else {
            message = afterLevel
          }
        } else {
          message = afterLevel
        }
      } else if line.hasPrefix("[") {
        if let closeBracket = line.firstIndex(of: "]") {
          let lvlSub = line[line.index(after: line.startIndex)..<closeBracket].trimmingCharacters(in: .whitespaces).uppercased()
          if ["ERROR", "WARN", "WARNING", "INFO", "DEBUG", "TRACE"].contains(lvlSub) {
            level = lvlSub == "WARNING" ? "WARN" : lvlSub
            let rem = line[line.index(after: closeBracket)...].trimmingCharacters(in: .whitespaces)
            if let colonIdx = rem.firstIndex(of: ":") {
              label = String(rem[..<colonIdx]).trimmingCharacters(in: .whitespaces)
              message = String(rem[rem.index(after: colonIdx)...]).trimmingCharacters(in: .whitespaces)
            } else {
              message = rem
            }
          }
        }
      } else if !isErr {
        level = "INFO"
      }

      cb?(level, label, message)
    }
  }
}
