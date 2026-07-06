// WebSocket control server
// Provides runtime control API compatible with the control protocol

import DSPConfig
import Foundation
import Network
import os

private enum WsResult {
  case ok
  case invalidFaderError
  case configValidationError(String)
  case configReadError(String)
  case invalidValueError(String)
  case invalidRequestError(String)
  case deviceNotFoundError(String)
  case deviceBusyError(String)
  case deviceError(String)
  case processingNotRunningError
}

private func jsonResult(_ result: WsResult) -> String {
  switch result {
  case .ok:
    return "\"Ok\""
  case .invalidFaderError:
    return "\"InvalidFaderError\""
  case .configValidationError(let msg):
    return "{\"ConfigValidationError\":\"\(msg)\"}"
  case .configReadError(let msg):
    return "{\"ConfigReadError\":\"\(msg)\"}"
  case .invalidValueError(let msg):
    return "{\"InvalidValueError\":\"\(msg)\"}"
  case .invalidRequestError(let msg):
    return "{\"InvalidRequestError\":\"\(msg)\"}"
  case .deviceNotFoundError(let msg):
    return "{\"DeviceNotFoundError\":\"\(msg)\"}"
  case .deviceBusyError(let msg):
    return "{\"DeviceBusyError\":\"\(msg)\"}"
  case .deviceError(let msg):
    return "{\"DeviceError\":\"\(msg)\"}"
  case .processingNotRunningError:
    return "\"ProcessingNotRunningError\""
  }
}

private func jsonReply(_ command: String, result: WsResult, value: String? = nil) -> String {
  let resStr = jsonResult(result)
  if let value = value {
    return "{\"\(command)\":{\"result\":\(resStr),\"value\":\(value)}}"
  }
  return "{\"\(command)\":{\"result\":\(resStr)}}"
}

private func jsonArray(_ values: [Double]) -> String {
  return "[\(values.map { String($0) }.joined(separator: ","))]"
}

private func dbToAmplitude(_ db: Double) -> Double {
  if db <= -100.0 { return 0.0 }
  return pow(10.0, db / 20.0)
}

private func amplitudeToDb(_ amp: Double) -> Double {
  if amp <= 0.00001 { return -100.0 }
  return 20.0 * log10(amp)
}

private func smoothingAlpha(dtMs: Double, timeConstantMs: Double) -> Double {
  if timeConstantMs <= 0.0 { return 1.0 }
  return 1.0 - exp(-dtMs / timeConstantMs)
}

private struct LevelSample: Sendable {
  var timestampMs: UInt64
  var levels: [Double]
}

private struct LevelHistory: Sendable {
  private var samples: [LevelSample] = []
  private var head: Int = 0
  private var count: Int = 0
  private(set) var channels: Int = 0

  mutating func reset(channels: Int) {
    self.channels = channels
    self.samples = Array(
      repeating: LevelSample(timestampMs: 0, levels: Array(repeating: -100.0, count: channels)),
      count: 300)
    self.head = 0
    self.count = 0
  }

  mutating func append(levels: [Double], timestampMs: UInt64) {
    guard channels > 0 else { return }
    let safeLevels: [Double]
    if levels.count < channels {
      safeLevels = levels + Array(repeating: -100.0, count: channels - levels.count)
    } else {
      safeLevels = Array(levels.prefix(channels))
    }
    samples[head] = LevelSample(timestampMs: timestampMs, levels: safeLevels)
    head = (head + 1) % 300
    count = min(count + 1, 300)
  }

  func getRmsSince(timestampMs: UInt64) -> [Double] {
    guard count > 0 && channels > 0 else { return Array(repeating: -100.0, count: channels) }
    var sumAmps = Array(repeating: 0.0, count: channels)
    var matchCount = 0

    for i in 0..<count {
      let idx = (head - 1 - i + 300) % 300
      let sample = samples[idx]
      if sample.timestampMs < timestampMs { break }
      for k in 0..<channels {
        let amp = dbToAmplitude(sample.levels[k])
        sumAmps[k] += amp * amp
      }
      matchCount += 1
    }

    if matchCount > 0 {
      var result = Array(repeating: 0.0, count: channels)
      for k in 0..<channels {
        result[k] = amplitudeToDb(sqrt(sumAmps[k] / Double(matchCount)))
      }
      return result
    } else {
      let latestIdx = (head - 1 + 300) % 300
      return samples[latestIdx].levels
    }
  }

  func getMaxSince(timestampMs: UInt64) -> [Double] {
    guard count > 0 && channels > 0 else { return Array(repeating: -100.0, count: channels) }
    var maxAmps = Array(repeating: 0.0, count: channels)
    var matchCount = 0

    for i in 0..<count {
      let idx = (head - 1 - i + 300) % 300
      let sample = samples[idx]
      if sample.timestampMs < timestampMs { break }
      for k in 0..<channels {
        let amp = dbToAmplitude(sample.levels[k])
        if amp > maxAmps[k] {
          maxAmps[k] = amp
        }
      }
      matchCount += 1
    }

    if matchCount > 0 {
      var result = Array(repeating: 0.0, count: channels)
      for k in 0..<channels {
        result[k] = amplitudeToDb(maxAmps[k])
      }
      return result
    } else {
      let latestIdx = (head - 1 + 300) % 300
      return samples[latestIdx].levels
    }
  }
}

public final class WebSocketServer: Sendable {
  private let logger = Logger(label: "dsp.websocket")
  private let port: UInt16
  private let host: String
  private let activePath: ActiveConfigPath

  // Connection management state protected by a lock
  private let stateLock = OSAllocatedUnfairLock(initialState: State())

  private struct ConnectionSubscription: Sendable {
    var lastCapPeakTime: UInt64 = 0
    var lastCapRmsTime: UInt64 = 0
    var lastPbPeakTime: UInt64 = 0
    var lastPbRmsTime: UInt64 = 0

    var stateSubscribed: Bool = false
    var vuSubscribed: Bool = false
    var signalLevelsSubscribed: Bool = false
    var signalLevelsSide: String = ""

    var vuMaxRate: Double = 0.0
    var vuAttack: Double = 0.0
    var vuRelease: Double = 0.0

    var lastVuPushTime: UInt64 = 0
    var lastSignalLevelsPushTime: UInt64 = 0

    var lastState: String = ""

    var vuPbRms: [Double] = []
    var vuPbPeak: [Double] = []
    var vuCapRms: [Double] = []
    var vuCapPeak: [Double] = []
    var vuPbChannels: Int = 0
    var vuCapChannels: Int = 0
  }

  private struct State {
    var listener: NWListener?
    var connections: [NWConnection] = []
    var subscriptions: [ObjectIdentifier: ConnectionSubscription] = [:]
    var stateFilePath: String?
    var previousConfig: String?
    var unsavedStateChanges: Bool = false
    var activeConfig: DSPConfiguration?
    var activeConfigTitle: String?
    var activeConfigDescription: String?
    var engine: SwiftDSPEngine?
    var broadcastTask: Task<Void, Never>?

    var updateInterval: UInt32 = 100

    var capturePeakHistory = LevelHistory()
    var captureRmsHistory = LevelHistory()
    var playbackPeakHistory = LevelHistory()
    var playbackRmsHistory = LevelHistory()

    var captureGlobalPeaks: [Double] = []
    var playbackGlobalPeaks: [Double] = []
  }

  public init(
    port: UInt16, host: String = "127.0.0.1", activePath: ActiveConfigPath,
    stateFilePath: String? = nil
  ) {
    self.port = port
    self.host = host
    self.activePath = activePath
    stateLock.withLock { state in
      state.stateFilePath = stateFilePath
    }
  }

  public func clearUnsavedStateChanges() {
    stateLock.withLock { state in
      state.unsavedStateChanges = false
    }
  }

  public func setEngine(_ engine: SwiftDSPEngine) {
    stateLock.withLock { state in
      state.engine = engine
    }
    // Fetch initial active configuration asynchronously
    Task {
      let config = await engine.getActiveConfig()
      stateLock.withLock { state in
        state.activeConfig = config
        if let config = config, let dict = try? jsonFromConfig(config) {
          state.activeConfigTitle = dict["title"] as? String
          state.activeConfigDescription = dict["description"] as? String
        }
      }
    }
  }

  public func start() throws {
    let params = NWParameters(tls: nil)
    let wsOptions = NWProtocolWebSocket.Options()
    params.defaultProtocolStack.applicationProtocols.insert(wsOptions, at: 0)

    let listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: port)!)

    listener.stateUpdateHandler = { [weak self] state in
      switch state {
      case .ready:
        if let self = self {
          self.logger.info(
            "WebSocket server listening on %s:%d", .string(self.host), .int(Int(self.port)))
        }
      case .failed(let error):
        self?.logger.error("WebSocket server failed: %s", .string(error.localizedDescription))
      default:
        break
      }
    }

    listener.newConnectionHandler = { [weak self] connection in
      self?.handleNewConnection(connection)
    }

    stateLock.withLock { state in
      state.listener = listener
      state.broadcastTask = Task { [weak self] in
        while !Task.isCancelled {
          let interval = self?.stateLock.withLock { $0.updateInterval } ?? 100
          try? await Task.sleep(nanoseconds: UInt64(interval) * 1_000_000)
          await self?.broadcastTick()
        }
      }
    }
    listener.start(queue: DispatchQueue(label: "camilladsp.websocket.listener"))
  }

  public func stop() {
    stateLock.withLock { state in
      state.listener?.cancel()
      state.listener = nil
      state.broadcastTask?.cancel()
      state.broadcastTask = nil
      state.connections.forEach { $0.cancel() }
      state.connections.removeAll()
      state.subscriptions.removeAll()
    }
    logger.info("WebSocket server stopped")
  }

  private func handleNewConnection(_ connection: NWConnection) {
    stateLock.withLock { state in
      state.connections.append(connection)
    }

    connection.stateUpdateHandler = { [weak self] state in
      if case .cancelled = state {
        self?.stateLock.withLock { state in
          let id = ObjectIdentifier(connection)
          state.connections.removeAll { $0 === connection }
          state.subscriptions.removeValue(forKey: id)
        }
      }
    }

    connection.start(queue: DispatchQueue(label: "camilladsp.websocket.connection"))
    receiveMessage(from: connection)
  }

  private func receiveMessage(from connection: NWConnection) {
    connection.receiveMessage { [weak self] data, context, isComplete, error in
      guard let self = self, let data = data else { return }

      if let message = context?.protocolMetadata(definition: NWProtocolWebSocket.definition)
        as? NWProtocolWebSocket.Metadata,
        message.opcode == .text
      {
        if let text = String(data: data, encoding: .utf8) {
          Task {
            let response = await self.handleCommand(connection: connection, commandText: text)
            self.send(response, to: connection)
          }
        }
      }

      // Continue receiving
      self.receiveMessage(from: connection)
    }
  }

  private func send(_ text: String, to connection: NWConnection) {
    let data = text.data(using: .utf8)!
    let metadata = NWProtocolWebSocket.Metadata(opcode: .text)
    let context = NWConnection.ContentContext(identifier: "ws", metadata: [metadata])
    connection.send(
      content: data, contentContext: context, isComplete: true, completion: .idempotent)
  }

  private func broadcastTick() async {
    let engine = stateLock.withLock { $0.engine }
    guard let engine = engine else { return }

    let status = await engine.getStatus()
    let processingParams = await engine.getProcessingParameters()

    let now = UInt64(Date().timeIntervalSince1970 * 1000)

    var currentCapPeak: [Double]?
    var currentCapRms: [Double]?
    var currentPbPeak: [Double]?
    var currentPbRms: [Double]?
    var capChannels = 0
    var pbChannels = 0

    if let params = processingParams {
      capChannels = params.captureChannels
      pbChannels = params.playbackChannels

      if capChannels > 0 {
        currentCapPeak = params.captureSignalPeak
        currentCapRms = params.captureSignalRms
      }
      if pbChannels > 0 {
        currentPbPeak = params.playbackSignalPeak
        currentPbRms = params.playbackSignalRms
      }
    }

    let capChannelsConst = capChannels
    let pbChannelsConst = pbChannels
    let currentCapPeakConst = currentCapPeak
    let currentCapRmsConst = currentCapRms
    let currentPbPeakConst = currentPbPeak
    let currentPbRmsConst = currentPbRms

    stateLock.withLock { state in
      if let capPeak = currentCapPeakConst, let capRms = currentCapRmsConst {
        if state.capturePeakHistory.channels != capChannelsConst {
          state.capturePeakHistory.reset(channels: capChannelsConst)
          state.captureRmsHistory.reset(channels: capChannelsConst)
        }
        state.capturePeakHistory.append(levels: capPeak, timestampMs: now)
        state.captureRmsHistory.append(levels: capRms, timestampMs: now)

        if state.captureGlobalPeaks.count != capChannelsConst {
          state.captureGlobalPeaks = Array(repeating: -1000.0, count: capChannelsConst)
        }
        for k in 0..<capChannelsConst {
          if capPeak[k] > state.captureGlobalPeaks[k] {
            state.captureGlobalPeaks[k] = capPeak[k]
          }
        }
      }

      if let pbPeak = currentPbPeakConst, let pbRms = currentPbRmsConst {
        if state.playbackPeakHistory.channels != pbChannelsConst {
          state.playbackPeakHistory.reset(channels: pbChannelsConst)
          state.playbackRmsHistory.reset(channels: pbChannelsConst)
        }
        state.playbackPeakHistory.append(levels: pbPeak, timestampMs: now)
        state.playbackRmsHistory.append(levels: pbRms, timestampMs: now)

        if state.playbackGlobalPeaks.count != pbChannelsConst {
          state.playbackGlobalPeaks = Array(repeating: -1000.0, count: pbChannelsConst)
        }
        for k in 0..<pbChannelsConst {
          if pbPeak[k] > state.playbackGlobalPeaks[k] {
            state.playbackGlobalPeaks[k] = pbPeak[k]
          }
        }
      }
    }

    let stateStr: String
    switch status.state {
    case .starting: stateStr = "Starting"
    case .running: stateStr = "Running"
    case .paused: stateStr = "Paused"
    case .stalled: stateStr = "Stalled"
    case .inactive: stateStr = "Inactive"
    }

    let reasonStr: String
    switch status.stopReason {
    case .none: reasonStr = "None"
    case .done: reasonStr = "Done"
    case .captureError(let msg): reasonStr = "CaptureError: \(msg)"
    case .playbackError(let msg): reasonStr = "PlaybackError: \(msg)"
    case .captureFormatChange(let rate): reasonStr = "CaptureFormatChange(\(rate))"
    case .playbackFormatChange(let rate): reasonStr = "PlaybackFormatChange(\(rate))"
    case .unknownError(let msg): reasonStr = "UnknownError: \(msg)"
    }

    let stateValueJSON = "{\"state\":\"\(stateStr)\",\"stop_reason\":\"\(reasonStr)\"}"

    let connectionsToNotify = stateLock.withLock { state -> [(NWConnection, String)] in
      var list: [(NWConnection, String)] = []
      for conn in state.connections {
        let id = ObjectIdentifier(conn)
        var sub = state.subscriptions[id] ?? ConnectionSubscription()

        if sub.stateSubscribed && sub.lastState != stateStr {
          sub.lastState = stateStr
          let msg = "{\"StateEvent\":{\"result\":\"Ok\",\"value\":\(stateValueJSON)}}"
          list.append((conn, msg))
        }

        if sub.vuSubscribed && pbChannelsConst > 0 {
          let interval = sub.vuMaxRate > 0.0 ? 1000.0 / sub.vuMaxRate : 0.0
          if Double(now - sub.lastVuPushTime) >= interval {
            let dt = sub.lastVuPushTime == 0 ? 100.0 : Double(now - sub.lastVuPushTime)
            let attack = smoothingAlpha(dtMs: dt, timeConstantMs: sub.vuAttack)
            let release = smoothingAlpha(dtMs: dt, timeConstantMs: sub.vuRelease)

            if sub.vuPbRms.count != pbChannelsConst {
              sub.vuPbRms = currentPbRmsConst ?? Array(repeating: -100.0, count: pbChannelsConst)
              sub.vuPbPeak = currentPbPeakConst ?? Array(repeating: -100.0, count: pbChannelsConst)
            } else if let pbRms = currentPbRmsConst, let pbPeak = currentPbPeakConst {
              for k in 0..<pbChannelsConst {
                let prevAmp = dbToAmplitude(sub.vuPbRms[k])
                let currAmp = dbToAmplitude(pbRms[k])
                let diff = currAmp - prevAmp
                sub.vuPbRms[k] = amplitudeToDb(prevAmp + (diff > 0 ? attack : release) * diff)
              }
              for k in 0..<pbChannelsConst {
                let prevAmp = dbToAmplitude(sub.vuPbPeak[k])
                let currAmp = dbToAmplitude(pbPeak[k])
                let diff = currAmp - prevAmp
                sub.vuPbPeak[k] = amplitudeToDb(prevAmp + (diff > 0 ? 1.0 : release) * diff)
              }
            }

            if capChannelsConst > 0 {
              if sub.vuCapRms.count != capChannelsConst {
                sub.vuCapRms =
                  currentCapRmsConst ?? Array(repeating: -100.0, count: capChannelsConst)
                sub.vuCapPeak =
                  currentCapPeakConst ?? Array(repeating: -100.0, count: capChannelsConst)
              } else if let capRms = currentCapRmsConst, let capPeak = currentCapPeakConst {
                for k in 0..<capChannelsConst {
                  let prevAmp = dbToAmplitude(sub.vuCapRms[k])
                  let currAmp = dbToAmplitude(capRms[k])
                  let diff = currAmp - prevAmp
                  sub.vuCapRms[k] = amplitudeToDb(prevAmp + (diff > 0 ? attack : release) * diff)
                }
                for k in 0..<capChannelsConst {
                  let prevAmp = dbToAmplitude(sub.vuCapPeak[k])
                  let currAmp = dbToAmplitude(capPeak[k])
                  let diff = currAmp - prevAmp
                  sub.vuCapPeak[k] = amplitudeToDb(prevAmp + (diff > 0 ? 1.0 : release) * diff)
                }
              }
            }

            let pRmsStr = jsonArray(sub.vuPbRms)
            let pPkStr = jsonArray(sub.vuPbPeak)
            let cRmsStr = jsonArray(sub.vuCapRms)
            let cPkStr = jsonArray(sub.vuCapPeak)
            let msg =
              "{\"VuLevelsEvent\":{\"result\":\"Ok\",\"value\":{\"playback_rms\":\(pRmsStr),\"playback_peak\":\(pPkStr),\"capture_rms\":\(cRmsStr),\"capture_peak\":\(cPkStr)}}}"
            list.append((conn, msg))
            sub.lastVuPushTime = now
          }
        }

        if sub.signalLevelsSubscribed {
          let sendPb = sub.signalLevelsSide == "playback" || sub.signalLevelsSide == "both"
          let sendCap = sub.signalLevelsSide == "capture" || sub.signalLevelsSide == "both"

          if sendPb, let pbRms = currentPbRmsConst, let pbPeak = currentPbPeakConst {
            let pRmsStr = jsonArray(pbRms)
            let pPkStr = jsonArray(pbPeak)
            let msg =
              "{\"SignalLevelsEvent\":{\"result\":\"Ok\",\"value\":{\"side\":\"playback\",\"rms\":\(pRmsStr),\"peak\":\(pPkStr)}}}"
            list.append((conn, msg))
          }
          if sendCap, let capRms = currentCapRmsConst, let capPeak = currentCapPeakConst {
            let cRmsStr = jsonArray(capRms)
            let cPkStr = jsonArray(capPeak)
            let msg =
              "{\"SignalLevelsEvent\":{\"result\":\"Ok\",\"value\":{\"side\":\"capture\",\"rms\":\(cRmsStr),\"peak\":\(cPkStr)}}}"
            list.append((conn, msg))
          }
        }

        state.subscriptions[id] = sub
      }
      return list
    }

    for (conn, msg) in connectionsToNotify {
      send(msg, to: conn)
    }
  }

  // MARK: - Command Handler

  private func handleCommand(connection: NWConnection, commandText: String) async -> String {
    let trimmed = commandText.trimmingCharacters(in: .whitespacesAndNewlines)

    // Simple string commands (quoted, e.g. "GetVersion")
    let simpleCommand = trimmed.trimmingCharacters(in: CharacterSet(charactersIn: "\""))

    let engine = stateLock.withLock { $0.engine }
    let processingParams = await engine?.getProcessingParameters()

    switch simpleCommand {
    case "GetVersion":
      return jsonReply("GetVersion", result: .ok, value: "\"CamillaDSP-Swift-Embedded 2.0.0\"")

    case "GetState":
      guard let status = await engine?.getStatus() else {
        return jsonReply("GetState", result: .invalidRequestError("Engine not available"))
      }
      let stateStr: String
      switch status.state {
      case .starting: stateStr = "Starting"
      case .running: stateStr = "Running"
      case .paused: stateStr = "Paused"
      case .stalled: stateStr = "Stalled"
      case .inactive: stateStr = "Inactive"
      }
      return jsonReply("GetState", result: .ok, value: "\"\(stateStr)\"")

    case "GetStopReason":
      guard let status = await engine?.getStatus() else {
        return jsonReply("GetStopReason", result: .invalidRequestError("Engine not available"))
      }
      let reasonStr: String
      switch status.stopReason {
      case .none: reasonStr = "None"
      case .done: reasonStr = "Done"
      case .captureError(let msg): reasonStr = "CaptureError: \(msg)"
      case .playbackError(let msg): reasonStr = "PlaybackError: \(msg)"
      case .captureFormatChange(let rate): reasonStr = "CaptureFormatChange(\(rate))"
      case .playbackFormatChange(let rate): reasonStr = "PlaybackFormatChange(\(rate))"
      case .unknownError(let msg): reasonStr = "UnknownError: \(msg)"
      }
      return jsonReply("GetStopReason", result: .ok, value: "\"\(reasonStr)\"")

    case "GetVolume":
      guard let params = processingParams else {
        return jsonReply("GetVolume", result: .processingNotRunningError)
      }
      return jsonReply("GetVolume", result: .ok, value: "\(params.targetVolume(for: .main))")

    case "GetMute":
      guard let params = processingParams else {
        return jsonReply("GetMute", result: .processingNotRunningError)
      }
      return jsonReply("GetMute", result: .ok, value: "\(params.isMuted(for: .main))")

    case "ToggleMute":
      guard let params = processingParams else {
        return jsonReply("ToggleMute", result: .processingNotRunningError)
      }
      let wasMuted = params.isMuted(for: .main)
      params.setMuted(!wasMuted, for: .main)
      stateLock.withLock { $0.unsavedStateChanges = true }
      return jsonReply("ToggleMute", result: .ok, value: "\(!wasMuted)")

    case "GetFaders":
      guard let params = processingParams else {
        return jsonReply("GetFaders", result: .processingNotRunningError)
      }
      var faders: [String] = []
      for f in [Fader.main, .aux1, .aux2, .aux3, .aux4] {
        let v = params.targetVolume(for: f)
        let m = params.isMuted(for: f)
        faders.append("{\"volume\":\(v),\"mute\":\(m)}")
      }
      return jsonReply("GetFaders", result: .ok, value: "[\(faders.joined(separator: ","))]")

    case "GetCaptureSignalRms":
      guard let params = processingParams else {
        return jsonReply("GetCaptureSignalRms", result: .processingNotRunningError)
      }
      return jsonReply(
        "GetCaptureSignalRms", result: .ok, value: jsonArray(params.captureSignalRms))

    case "GetCaptureSignalPeak":
      guard let params = processingParams else {
        return jsonReply("GetCaptureSignalPeak", result: .processingNotRunningError)
      }
      return jsonReply(
        "GetCaptureSignalPeak", result: .ok, value: jsonArray(params.captureSignalPeak))

    case "GetPlaybackSignalRms":
      guard let params = processingParams else {
        return jsonReply("GetPlaybackSignalRms", result: .processingNotRunningError)
      }
      return jsonReply(
        "GetPlaybackSignalRms", result: .ok, value: jsonArray(params.playbackSignalRms))

    case "GetPlaybackSignalPeak":
      guard let params = processingParams else {
        return jsonReply("GetPlaybackSignalPeak", result: .processingNotRunningError)
      }
      return jsonReply(
        "GetPlaybackSignalPeak", result: .ok, value: jsonArray(params.playbackSignalPeak))

    case "GetCaptureRate":
      guard let status = await engine?.getStatus(), status.state == .running else {
        return jsonReply("GetCaptureRate", result: .ok, value: "0")
      }
      let config = stateLock.withLock { $0.activeConfig }
      return jsonReply("GetCaptureRate", result: .ok, value: "\(config?.devices.samplerate ?? 0)")

    case "GetRateAdjust":
      guard let params = processingParams else {
        return jsonReply("GetRateAdjust", result: .ok, value: "1.0")
      }
      return jsonReply("GetRateAdjust", result: .ok, value: "\(params.rateAdjust.value)")

    case "GetBufferLevel":
      guard let params = processingParams else {
        return jsonReply("GetBufferLevel", result: .ok, value: "0")
      }
      return jsonReply("GetBufferLevel", result: .ok, value: "\(Int(params.bufferLevel.value))")

    case "GetClippedSamples":
      guard let params = processingParams else {
        return jsonReply("GetClippedSamples", result: .ok, value: "0")
      }
      return jsonReply(
        "GetClippedSamples", result: .ok, value: "\(params.clippedSamples.load(ordering: .relaxed))"
      )

    case "ResetClippedSamples":
      processingParams?.clippedSamples.store(0, ordering: .relaxed)
      return jsonReply("ResetClippedSamples", result: .ok)

    case "GetProcessingLoad":
      guard let params = processingParams else {
        return jsonReply("GetProcessingLoad", result: .ok, value: "0.0")
      }
      return jsonReply("GetProcessingLoad", result: .ok, value: "\(params.processingLoad.value)")

    case "GetResamplerLoad":
      guard let params = processingParams else {
        return jsonReply("GetResamplerLoad", result: .ok, value: "0.0")
      }
      return jsonReply("GetResamplerLoad", result: .ok, value: "\(params.resamplerLoad.value)")

    case "GetSupportedDeviceTypes":
      return jsonReply(
        "GetSupportedDeviceTypes", result: .ok, value: "[[\"CoreAudio\"],[\"CoreAudio\"]]")

    case "GetConfigFilePath":
      let path = activePath.value
      return jsonReply(
        "GetConfigFilePath", result: .ok, value: path.map { "\"\($0)\"" } ?? "null")

    case "GetPreviousConfig":
      let prev = stateLock.withLock { $0.previousConfig }
      return jsonReply("GetPreviousConfig", result: .ok, value: prev.map { "\"\($0)\"" } ?? "null")

    case "GetStateFilePath":
      let path = stateLock.withLock { $0.stateFilePath }
      return jsonReply("GetStateFilePath", result: .ok, value: path.map { "\"\($0)\"" } ?? "null")

    case "GetStateFileUpdated":
      let unsaved = stateLock.withLock { $0.unsavedStateChanges }
      return jsonReply("GetStateFileUpdated", result: .ok, value: "\(!unsaved)")

    case "GetConfig":
      guard let active = stateLock.withLock({ $0.activeConfig }) else {
        return jsonReply(
          "GetConfig", result: .invalidRequestError("No active config"),
          value: "\"No active config\"")
      }
      do {
        let encoder = JSONEncoder()
        encoder.outputFormatting = .prettyPrinted
        let data = try encoder.encode(active)
        let jsonStr = String(data: data, encoding: .utf8) ?? "{}"
        return jsonReply("GetConfig", result: .ok, value: jsonStr)
      } catch {
        return jsonReply("GetConfig", result: .configReadError(error.localizedDescription))
      }

    case "GetConfigJson":
      guard let active = stateLock.withLock({ $0.activeConfig }) else {
        return jsonReply(
          "GetConfigJson", result: .invalidRequestError("No active config"),
          value: "\"No active config\"")
      }
      do {
        let data = try JSONEncoder().encode(active)
        let jsonStr = String(data: data, encoding: .utf8) ?? "{}"
        return jsonReply("GetConfigJson", result: .ok, value: jsonStr)
      } catch {
        return jsonReply("GetConfigJson", result: .configReadError(error.localizedDescription))
      }

    case "GetConfigTitle":
      let title = stateLock.withLock { $0.activeConfigTitle }
      return jsonReply(
        "GetConfigTitle", result: .ok, value: title.map { "\"\($0)\"" } ?? "null")

    case "GetConfigDescription":
      let desc = stateLock.withLock { $0.activeConfigDescription }
      return jsonReply(
        "GetConfigDescription", result: .ok, value: desc.map { "\"\($0)\"" } ?? "null")

    case "Reload":
      guard let path = activePath.value else {
        return jsonReply(
          "Reload", result: .invalidRequestError("No config file path set"),
          value: "\"No config file path set\"")
      }
      return await handleReloadFromPath(path)

    case "Stop":
      await engine?.stop()
      return jsonReply("Stop", result: .ok)

    case "Exit":
      await engine?.stop()
      return jsonReply("Exit", result: .ok)

    case "SubscribeState":
      stateLock.withLock { state in
        let id = ObjectIdentifier(connection)
        var sub = state.subscriptions[id] ?? ConnectionSubscription()
        sub.stateSubscribed = true
        state.subscriptions[id] = sub
      }
      return jsonReply("SubscribeState", result: .ok)

    case "SubscribeVuLevels":
      stateLock.withLock { state in
        let id = ObjectIdentifier(connection)
        var sub = state.subscriptions[id] ?? ConnectionSubscription()
        sub.vuSubscribed = true
        sub.vuMaxRate = 0.0
        sub.vuAttack = 0.0
        sub.vuRelease = 0.0
        sub.lastVuPushTime = 0
        state.subscriptions[id] = sub
      }
      return jsonReply("SubscribeVuLevels", result: .ok)

    case "StopSubscription":
      let found = stateLock.withLock { state in
        let id = ObjectIdentifier(connection)
        if let sub = state.subscriptions[id] {
          let active = sub.stateSubscribed || sub.vuSubscribed || sub.signalLevelsSubscribed
          if active {
            state.subscriptions.removeValue(forKey: id)
            return true
          }
        }
        return false
      }
      if found {
        return jsonReply("StopSubscription", result: .ok)
      } else {
        return jsonReply(
          "StopSubscription", result: .invalidRequestError("No active subscription to stop"))
      }

    case "GetCaptureSignalRmsSinceLast":
      guard processingParams != nil else {
        return jsonReply("GetCaptureSignalRmsSinceLast", result: .processingNotRunningError)
      }
      let rms = stateLock.withLock { state -> [Double] in
        let id = ObjectIdentifier(connection)
        var sub = state.subscriptions[id] ?? ConnectionSubscription()
        let since = sub.lastCapRmsTime
        sub.lastCapRmsTime = UInt64(Date().timeIntervalSince1970 * 1000)
        state.subscriptions[id] = sub
        return state.captureRmsHistory.getRmsSince(timestampMs: since)
      }
      return jsonReply("GetCaptureSignalRmsSinceLast", result: .ok, value: jsonArray(rms))

    case "GetCaptureSignalPeakSinceLast":
      guard processingParams != nil else {
        return jsonReply("GetCaptureSignalPeakSinceLast", result: .processingNotRunningError)
      }
      let pk = stateLock.withLock { state -> [Double] in
        let id = ObjectIdentifier(connection)
        var sub = state.subscriptions[id] ?? ConnectionSubscription()
        let since = sub.lastCapPeakTime
        sub.lastCapPeakTime = UInt64(Date().timeIntervalSince1970 * 1000)
        state.subscriptions[id] = sub
        return state.capturePeakHistory.getMaxSince(timestampMs: since)
      }
      return jsonReply("GetCaptureSignalPeakSinceLast", result: .ok, value: jsonArray(pk))

    case "GetPlaybackSignalRmsSinceLast":
      guard processingParams != nil else {
        return jsonReply("GetPlaybackSignalRmsSinceLast", result: .processingNotRunningError)
      }
      let rms = stateLock.withLock { state -> [Double] in
        let id = ObjectIdentifier(connection)
        var sub = state.subscriptions[id] ?? ConnectionSubscription()
        let since = sub.lastPbRmsTime
        sub.lastPbRmsTime = UInt64(Date().timeIntervalSince1970 * 1000)
        state.subscriptions[id] = sub
        return state.playbackRmsHistory.getRmsSince(timestampMs: since)
      }
      return jsonReply("GetPlaybackSignalRmsSinceLast", result: .ok, value: jsonArray(rms))

    case "GetPlaybackSignalPeakSinceLast":
      guard processingParams != nil else {
        return jsonReply("GetPlaybackSignalPeakSinceLast", result: .processingNotRunningError)
      }
      let pk = stateLock.withLock { state -> [Double] in
        let id = ObjectIdentifier(connection)
        var sub = state.subscriptions[id] ?? ConnectionSubscription()
        let since = sub.lastPbPeakTime
        sub.lastPbPeakTime = UInt64(Date().timeIntervalSince1970 * 1000)
        state.subscriptions[id] = sub
        return state.playbackPeakHistory.getMaxSince(timestampMs: since)
      }
      return jsonReply("GetPlaybackSignalPeakSinceLast", result: .ok, value: jsonArray(pk))

    case "GetSignalLevels":
      guard let params = processingParams else {
        return jsonReply("GetSignalLevels", result: .processingNotRunningError)
      }
      let pRms = jsonArray(params.playbackSignalRms)
      let pPk = jsonArray(params.playbackSignalPeak)
      let cRms = jsonArray(params.captureSignalRms)
      let cPk = jsonArray(params.captureSignalPeak)
      let val =
        "{\"playback_rms\":\(pRms),\"playback_peak\":\(pPk),\"capture_rms\":\(cRms),\"capture_peak\":\(cPk)}"
      return jsonReply("GetSignalLevels", result: .ok, value: val)

    case "GetSignalLevelsSinceLast":
      guard processingParams != nil else {
        return jsonReply("GetSignalLevelsSinceLast", result: .processingNotRunningError)
      }
      let (cRms, cPk, pRms, pPk) = stateLock.withLock {
        state -> ([Double], [Double], [Double], [Double]) in
        let id = ObjectIdentifier(connection)
        var sub = state.subscriptions[id] ?? ConnectionSubscription()
        let nowMs = UInt64(Date().timeIntervalSince1970 * 1000)
        let crSince = sub.lastCapRmsTime
        let cpSince = sub.lastCapPeakTime
        let prSince = sub.lastPbRmsTime
        let ppSince = sub.lastPbPeakTime

        sub.lastCapRmsTime = nowMs
        sub.lastCapPeakTime = nowMs
        sub.lastPbRmsTime = nowMs
        sub.lastPbPeakTime = nowMs
        state.subscriptions[id] = sub

        return (
          state.captureRmsHistory.getRmsSince(timestampMs: crSince),
          state.capturePeakHistory.getMaxSince(timestampMs: cpSince),
          state.playbackRmsHistory.getRmsSince(timestampMs: prSince),
          state.playbackPeakHistory.getMaxSince(timestampMs: ppSince)
        )
      }
      let val =
        "{\"playback_rms\":\(jsonArray(pRms)),\"playback_peak\":\(jsonArray(pPk)),\"capture_rms\":\(jsonArray(cRms)),\"capture_peak\":\(jsonArray(cPk))}"
      return jsonReply("GetSignalLevelsSinceLast", result: .ok, value: val)

    case "GetSignalPeaksSinceStart":
      let (cPeaks, pPeaks) = stateLock.withLock { ($0.captureGlobalPeaks, $0.playbackGlobalPeaks) }
      let val = "{\"capture\":\(jsonArray(cPeaks)),\"playback\":\(jsonArray(pPeaks))}"
      return jsonReply("GetSignalPeaksSinceStart", result: .ok, value: val)

    case "ResetSignalPeaksSinceStart":
      stateLock.withLock { state in
        for k in 0..<state.captureGlobalPeaks.count { state.captureGlobalPeaks[k] = -1000.0 }
        for k in 0..<state.playbackGlobalPeaks.count { state.playbackGlobalPeaks[k] = -1000.0 }
      }
      return jsonReply("ResetSignalPeaksSinceStart", result: .ok)

    case "GetChannelLabels":
      let active = stateLock.withLock { $0.activeConfig }
      let pLabels = active?.devices.playback.channelLabels ?? []
      let cLabels = active?.devices.capture.channelLabels ?? []
      let pStr =
        pLabels.isEmpty ? "null" : "[" + pLabels.map { "\"\($0)\"" }.joined(separator: ",") + "]"
      let cStr =
        cLabels.isEmpty ? "null" : "[" + cLabels.map { "\"\($0)\"" }.joined(separator: ",") + "]"
      return jsonReply(
        "GetChannelLabels", result: .ok, value: "{\"playback\":\(pStr),\"capture\":\(cStr)}")

    case "GetSignalRange":
      guard let params = processingParams else {
        return jsonReply("GetSignalRange", result: .processingNotRunningError)
      }
      let pbPeak = params.playbackSignalPeak
      let maxPeak = pbPeak.max() ?? -1000.0
      let range = 2.0 * dbToAmplitude(maxPeak)
      return jsonReply("GetSignalRange", result: .ok, value: "\(range)")

    case "GetUpdateInterval":
      let interval = stateLock.withLock { $0.updateInterval }
      return jsonReply("GetUpdateInterval", result: .ok, value: "\(interval)")

    default:
      // Try JSON object commands
      return await handleJSONCommand(connection: connection, jsonText: trimmed)
    }
  }

  private func handleReloadFromPath(_ path: String) async -> String {
    let url = URL(fileURLWithPath: path)
    do {
      let data = try Data(contentsOf: url)
      let jsonStr = String(data: data, encoding: .utf8) ?? ""
      try await stateLock.withLock { $0.engine }?.setConfig(json: jsonStr)
      let parsed = try JSONDecoder().decode(DSPConfiguration.self, from: data)
      stateLock.withLock { state in
        if let current = state.activeConfig,
          let currentData = try? JSONEncoder().encode(current)
        {
          state.previousConfig = String(data: currentData, encoding: .utf8)
        }
        state.activeConfig = parsed
        if let dict = try? jsonFromConfig(parsed) {
          state.activeConfigTitle = dict["title"] as? String
          state.activeConfigDescription = dict["description"] as? String
        } else {
          state.activeConfigTitle = nil
          state.activeConfigDescription = nil
        }
        state.unsavedStateChanges = false
      }
      return jsonReply("Reload", result: .ok)
    } catch {
      return jsonReply("Reload", result: .configReadError(error.localizedDescription))
    }
  }

  private func handleJSONCommand(connection: NWConnection, jsonText: String) async -> String {
    guard let data = jsonText.data(using: .utf8),
      let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    else {
      return "{\"Invalid\":{\"error\":\"Invalid command: could not parse JSON\"}}"
    }

    let engine = stateLock.withLock { $0.engine }
    let processingParams = await engine?.getProcessingParameters()

    if let volume = json["SetVolume"] as? Double {
      guard let params = processingParams else {
        return jsonReply("SetVolume", result: .processingNotRunningError)
      }
      let clamped = min(50.0, max(-150.0, volume))
      params.setTargetVolume(clamped, for: .main)
      stateLock.withLock { $0.unsavedStateChanges = true }
      return jsonReply("SetVolume", result: .ok)
    }

    if let mute = json["SetMute"] as? Bool {
      guard let params = processingParams else {
        return jsonReply("SetMute", result: .processingNotRunningError)
      }
      params.setMuted(mute, for: .main)
      stateLock.withLock { $0.unsavedStateChanges = true }
      return jsonReply("SetMute", result: .ok)
    }

    if let path = json["SetConfigFilePath"] as? String {
      activePath.value = path
      return jsonReply("SetConfigFilePath", result: .ok)
    }

    if let configJson = json["SetConfigJson"] as? String {
      do {
        try await engine?.setConfig(json: configJson)
        let parsed = try JSONDecoder().decode(
          DSPConfiguration.self, from: configJson.data(using: .utf8)!)
        stateLock.withLock { state in
          if let current = state.activeConfig,
            let currentData = try? JSONEncoder().encode(current)
          {
            state.previousConfig = String(data: currentData, encoding: .utf8)
          }
          state.activeConfig = parsed
          if let dict = try? jsonFromConfig(parsed) {
            state.activeConfigTitle = dict["title"] as? String
            state.activeConfigDescription = dict["description"] as? String
          } else {
            state.activeConfigTitle = nil
            state.activeConfigDescription = nil
          }
          state.unsavedStateChanges = false
        }
        return jsonReply("SetConfigJson", result: .ok)
      } catch {
        return jsonReply(
          "SetConfigJson", result: .configValidationError(error.localizedDescription))
      }
    }

    if let pointer = json["GetConfigValue"] as? String {
      guard let active = stateLock.withLock({ $0.activeConfig }) else {
        return jsonReply(
          "GetConfigValue", result: .invalidRequestError("No active config"),
          value: "\"No active config\"")
      }
      do {
        let dict = try jsonFromConfig(active)
        if let val = getValueAtPointer(dict, pointer: pointer) {
          let valData = try JSONSerialization.data(withJSONObject: val)
          let valStr = String(data: valData, encoding: .utf8) ?? "null"
          return jsonReply("GetConfigValue", result: .ok, value: valStr)
        } else {
          return jsonReply(
            "GetConfigValue", result: .invalidRequestError("Path not found: \(pointer)"),
            value: "\"Path not found: \(pointer)\"")
        }
      } catch {
        return jsonReply(
          "GetConfigValue", result: .invalidRequestError("\(error)"), value: "\"\(error)\"")
      }
    }

    if let patchValue = json["SetConfigValue"] as? [String: Any],
      let pointer = patchValue["pointer"] as? String
        ?? (patchValue.keys.first.flatMap { $0 != "value" ? $0 : nil }),
      let newValue = patchValue["value"] ?? patchValue[pointer]
    {
      guard var config = stateLock.withLock({ $0.activeConfig }) else {
        return jsonReply(
          "SetConfigValue", result: .invalidRequestError("No active config to modify"),
          value: "\"No active config to modify\"")
      }
      do {
        var configJSON = try jsonFromConfig(config)
        if setValueAtPointer(&configJSON, pointer: pointer, value: newValue) {
          let data = try JSONSerialization.data(withJSONObject: configJSON)
          config = try JSONDecoder().decode(DSPConfiguration.self, from: data)
          let configStr = String(data: data, encoding: .utf8) ?? ""
          try await engine?.setConfig(json: configStr)
          let finalConfig = config
          stateLock.withLock { state in
            state.activeConfig = finalConfig
            if let dict = try? jsonFromConfig(finalConfig) {
              state.activeConfigTitle = dict["title"] as? String
              state.activeConfigDescription = dict["description"] as? String
            }
          }
          return jsonReply("SetConfigValue", result: .ok)
        } else {
          return jsonReply(
            "SetConfigValue", result: .invalidRequestError("Path not found: \(pointer)"),
            value: "\"Path not found: \(pointer)\"")
        }
      } catch {
        return jsonReply(
          "SetConfigValue", result: .invalidRequestError("\(error)"), value: "\"\(error)\"")
      }
    }

    if let patchData = json["PatchConfig"] {
      guard var config = stateLock.withLock({ $0.activeConfig }) else {
        return jsonReply(
          "PatchConfig", result: .invalidRequestError("No active config to patch"),
          value: "\"No active config to patch\"")
      }
      do {
        var configJSON = try jsonFromConfig(config)
        if let patch = patchData as? [String: Any] {
          mergeJSON(&configJSON, patch: patch)
        }
        let data = try JSONSerialization.data(withJSONObject: configJSON)
        config = try JSONDecoder().decode(DSPConfiguration.self, from: data)
        let configStr = String(data: data, encoding: .utf8) ?? ""
        try await engine?.setConfig(json: configStr)
        let finalConfig = config
        stateLock.withLock { state in
          state.activeConfig = finalConfig
          if let dict = try? jsonFromConfig(finalConfig) {
            state.activeConfigTitle = dict["title"] as? String
            state.activeConfigDescription = dict["description"] as? String
          }
        }
        return jsonReply("PatchConfig", result: .ok)
      } catch {
        return jsonReply(
          "PatchConfig", result: .invalidRequestError("\(error)"), value: "\"\(error)\"")
      }
    }

    if let idx = json["GetFaderVolume"] as? Int {
      guard let params = processingParams else {
        return jsonReply("GetFaderVolume", result: .processingNotRunningError)
      }
      guard let fader = faderForIndex(idx) else {
        return jsonReply(
          "GetFaderVolume", result: .invalidFaderError,
          value: "[\(idx),\(ProcessingParameters.defaultVolume)]")
      }
      let vol = params.targetVolume(for: fader)
      return jsonReply("GetFaderVolume", result: .ok, value: "[\(idx),\(vol)]")
    }

    if let arr = json["SetFaderVolume"] as? [Any], arr.count >= 2,
      let idx = arr[0] as? Int, let vol = arr[1] as? Double
    {
      guard let params = processingParams else {
        return jsonReply("SetFaderVolume", result: .processingNotRunningError)
      }
      guard let fader = faderForIndex(idx) else {
        return jsonReply("SetFaderVolume", result: .invalidFaderError)
      }
      let clamped = min(50.0, max(-150.0, vol))
      params.setTargetVolume(clamped, for: fader)
      stateLock.withLock { $0.unsavedStateChanges = true }
      return jsonReply("SetFaderVolume", result: .ok)
    }

    if let idx = json["GetFaderMute"] as? Int {
      guard let params = processingParams else {
        return jsonReply("GetFaderMute", result: .processingNotRunningError)
      }
      guard let fader = faderForIndex(idx) else {
        return jsonReply(
          "GetFaderMute", result: .invalidFaderError,
          value: "[\(idx),\(ProcessingParameters.defaultMute)]")
      }
      let muted = params.isMuted(for: fader)
      return jsonReply("GetFaderMute", result: .ok, value: "[\(idx),\(muted)]")
    }

    if let arr = json["SetFaderMute"] as? [Any], arr.count >= 2,
      let idx = arr[0] as? Int, let mute = arr[1] as? Bool
    {
      guard let params = processingParams else {
        return jsonReply("SetFaderMute", result: .processingNotRunningError)
      }
      guard let fader = faderForIndex(idx) else {
        return jsonReply("SetFaderMute", result: .invalidFaderError)
      }
      params.setMuted(mute, for: fader)
      stateLock.withLock { $0.unsavedStateChanges = true }
      return jsonReply("SetFaderMute", result: .ok)
    }

    if let idx = json["ToggleFaderMute"] as? Int {
      guard let params = processingParams else {
        return jsonReply("ToggleFaderMute", result: .processingNotRunningError)
      }
      guard let fader = faderForIndex(idx) else {
        return jsonReply(
          "ToggleFaderMute", result: .invalidFaderError,
          value: "[\(idx),\(ProcessingParameters.defaultMute)]")
      }
      let wasMuted = params.isMuted(for: fader)
      params.setMuted(!wasMuted, for: fader)
      stateLock.withLock { $0.unsavedStateChanges = true }
      return jsonReply("ToggleFaderMute", result: .ok, value: "[\(idx),\(!wasMuted)]")
    }

    if let backend = json["GetAvailableCaptureDevices"] as? String {
      let list = await engine?.getAvailableDevices(backend: backend, input: true) ?? []
      let valStr = "[" + list.map { "\"\($0.name)\"" }.joined(separator: ",") + "]"
      return jsonReply("GetAvailableCaptureDevices", result: .ok, value: valStr)
    }

    if let backend = json["GetAvailablePlaybackDevices"] as? String {
      let list = await engine?.getAvailableDevices(backend: backend, input: false) ?? []
      let valStr = "[" + list.map { "\"\($0.name)\"" }.joined(separator: ",") + "]"
      return jsonReply("GetAvailablePlaybackDevices", result: .ok, value: valStr)
    }

    if let adjustObj = json["AdjustVolume"] {
      return await handleAdjustVolume(adjustObj, fader: .main)
    }

    if let arr = json["AdjustFaderVolume"] as? [Any], arr.count >= 2,
      let idx = arr[0] as? Int
    {
      guard let fader = faderForIndex(idx) else {
        return jsonReply("AdjustFaderVolume", result: .invalidFaderError)
      }
      let adjustObj = arr[1]
      return await handleAdjustVolume(adjustObj, fader: fader)
    }

    if let arr = json["GetCaptureDeviceCapabilities"] as? [String], arr.count >= 2 {
      let backend = arr[0]
      let device = arr[1]
      if let desc = await engine?.getDeviceCapabilities(
        backend: backend, device: device, isCapture: true)
      {
        if let data = try? JSONEncoder().encode(desc) {
          let valStr = String(data: data, encoding: .utf8) ?? "null"
          return jsonReply("GetCaptureDeviceCapabilities", result: .ok, value: valStr)
        } else {
          return jsonReply(
            "GetCaptureDeviceCapabilities", result: .deviceError("Failed to encode capabilities"))
        }
      } else {
        return jsonReply("GetCaptureDeviceCapabilities", result: .deviceNotFoundError(device))
      }
    }

    if let arr = json["GetPlaybackDeviceCapabilities"] as? [String], arr.count >= 2 {
      let backend = arr[0]
      let device = arr[1]
      if let desc = await engine?.getDeviceCapabilities(
        backend: backend, device: device, isCapture: false)
      {
        if let data = try? JSONEncoder().encode(desc) {
          let valStr = String(data: data, encoding: .utf8) ?? "null"
          return jsonReply("GetPlaybackDeviceCapabilities", result: .ok, value: valStr)
        } else {
          return jsonReply(
            "GetPlaybackDeviceCapabilities", result: .deviceError("Failed to encode capabilities"))
        }
      } else {
        return jsonReply("GetPlaybackDeviceCapabilities", result: .deviceNotFoundError(device))
      }
    }

    if let reqObj = json["GetSpectrum"] as? [String: Any],
      let isCapture = reqObj["is_capture"] as? Bool ?? reqObj["isCapture"] as? Bool
    {
      let channel = reqObj["channel"] as? UInt32
      let minFreq = reqObj["min_freq"] as? Double ?? reqObj["minFreq"] as? Double ?? 20.0
      let maxFreq = reqObj["max_freq"] as? Double ?? reqObj["maxFreq"] as? Double ?? 20000.0
      let nBins = reqObj["n_bins"] as? UInt32 ?? reqObj["nBins"] as? UInt32 ?? 1024
      do {
        if let spectrum = try await engine?.getSpectrum(
          isCapture: isCapture, channel: channel, minFreq: minFreq, maxFreq: maxFreq, nBins: nBins)
        {
          let data = try JSONEncoder().encode(spectrum)
          let valStr = String(data: data, encoding: .utf8) ?? "null"
          return jsonReply("GetSpectrum", result: .ok, value: valStr)
        }
      } catch {
        return jsonReply("GetSpectrum", result: .deviceError(error.localizedDescription))
      }
    }

    if let configJson = json["ReadConfigJson"] as? String {
      do {
        let parsed = try JSONDecoder().decode(
          DSPConfiguration.self, from: configJson.data(using: .utf8)!)
        let data = try JSONEncoder().encode(parsed)
        let jsonStr = String(data: data, encoding: .utf8) ?? "{}"
        return jsonReply("ReadConfigJson", result: .ok, value: jsonStr)
      } catch {
        return jsonReply(
          "ReadConfigJson", result: .configValidationError(error.localizedDescription))
      }
    }

    if let configJson = json["ValidateConfigJson"] as? String {
      do {
        _ = try JSONDecoder().decode(
          DSPConfiguration.self, from: configJson.data(using: .utf8)!)
        return jsonReply("ValidateConfigJson", result: .ok)
      } catch {
        return jsonReply(
          "ValidateConfigJson", result: .configValidationError(error.localizedDescription))
      }
    }

    if let subVuObj = json["SubscribeVuLevels"] as? [String: Any] {
      let maxRate = subVuObj["max_rate"] as? Double ?? subVuObj["maxRate"] as? Double ?? 0.0
      let attack = subVuObj["attack"] as? Double ?? 0.0
      let release = subVuObj["release"] as? Double ?? 0.0
      if attack < 0.0 || attack > 60000.0 || release < 0.0 || release > 60000.0 {
        return jsonReply(
          "SubscribeVuLevels",
          result: .invalidValueError("attack and release must be between 0 and 60000 ms"))
      }
      stateLock.withLock { state in
        let id = ObjectIdentifier(connection)
        var sub = state.subscriptions[id] ?? ConnectionSubscription()
        sub.vuSubscribed = true
        sub.vuMaxRate = maxRate
        sub.vuAttack = attack
        sub.vuRelease = release
        sub.lastVuPushTime = 0
        state.subscriptions[id] = sub
      }
      return jsonReply("SubscribeVuLevels", result: .ok)
    }

    if let side = json["SubscribeSignalLevels"] as? String {
      if side == "playback" || side == "capture" || side == "both" {
        stateLock.withLock { state in
          let id = ObjectIdentifier(connection)
          var sub = state.subscriptions[id] ?? ConnectionSubscription()
          sub.signalLevelsSubscribed = true
          sub.signalLevelsSide = side
          sub.lastSignalLevelsPushTime = 0
          state.subscriptions[id] = sub
        }
        return jsonReply("SubscribeSignalLevels", result: .ok)
      } else {
        return jsonReply(
          "SubscribeSignalLevels",
          result: .invalidValueError("side must be playback, capture, or both"))
      }
    }

    if let secs = json["GetCaptureSignalRmsSince"] as? Double {
      guard processingParams != nil else {
        return jsonReply("GetCaptureSignalRmsSince", result: .processingNotRunningError)
      }
      let since = UInt64(Date().timeIntervalSince1970 * 1000) - UInt64(secs * 1000.0)
      let rms = stateLock.withLock { $0.captureRmsHistory.getRmsSince(timestampMs: since) }
      return jsonReply("GetCaptureSignalRmsSince", result: .ok, value: jsonArray(rms))
    }

    if let secs = json["GetCaptureSignalPeakSince"] as? Double {
      guard processingParams != nil else {
        return jsonReply("GetCaptureSignalPeakSince", result: .processingNotRunningError)
      }
      let since = UInt64(Date().timeIntervalSince1970 * 1000) - UInt64(secs * 1000.0)
      let pk = stateLock.withLock { $0.capturePeakHistory.getMaxSince(timestampMs: since) }
      return jsonReply("GetCaptureSignalPeakSince", result: .ok, value: jsonArray(pk))
    }

    if let secs = json["GetPlaybackSignalRmsSince"] as? Double {
      guard processingParams != nil else {
        return jsonReply("GetPlaybackSignalRmsSince", result: .processingNotRunningError)
      }
      let since = UInt64(Date().timeIntervalSince1970 * 1000) - UInt64(secs * 1000.0)
      let rms = stateLock.withLock { $0.playbackRmsHistory.getRmsSince(timestampMs: since) }
      return jsonReply("GetPlaybackSignalRmsSince", result: .ok, value: jsonArray(rms))
    }

    if let secs = json["GetPlaybackSignalPeakSince"] as? Double {
      guard processingParams != nil else {
        return jsonReply("GetPlaybackSignalPeakSince", result: .processingNotRunningError)
      }
      let since = UInt64(Date().timeIntervalSince1970 * 1000) - UInt64(secs * 1000.0)
      let pk = stateLock.withLock { $0.playbackPeakHistory.getMaxSince(timestampMs: since) }
      return jsonReply("GetPlaybackSignalPeakSince", result: .ok, value: jsonArray(pk))
    }

    if let secs = json["GetSignalLevelsSince"] as? Double {
      guard processingParams != nil else {
        return jsonReply("GetSignalLevelsSince", result: .processingNotRunningError)
      }
      let since = UInt64(Date().timeIntervalSince1970 * 1000) - UInt64(secs * 1000.0)
      let (cRms, cPk, pRms, pPk) = stateLock.withLock { state in
        (
          state.captureRmsHistory.getRmsSince(timestampMs: since),
          state.capturePeakHistory.getMaxSince(timestampMs: since),
          state.playbackRmsHistory.getRmsSince(timestampMs: since),
          state.playbackPeakHistory.getMaxSince(timestampMs: since)
        )
      }
      let val =
        "{\"playback_rms\":\(jsonArray(pRms)),\"playback_peak\":\(jsonArray(pPk)),\"capture_rms\":\(jsonArray(cRms)),\"capture_peak\":\(jsonArray(cPk))}"
      return jsonReply("GetSignalLevelsSince", result: .ok, value: val)
    }

    if let interval = json["SetUpdateInterval"] as? Int {
      if interval >= 10 && interval <= 10000 {
        stateLock.withLock { $0.updateInterval = UInt32(interval) }
        return jsonReply("SetUpdateInterval", result: .ok)
      } else {
        return jsonReply(
          "SetUpdateInterval",
          result: .invalidValueError("update interval must be between 10 and 10000 ms"))
      }
    }

    return "{\"Invalid\":{\"error\":\"Unsupported JSON command\"}}"
  }

  private func handleAdjustVolume(_ arg: Any, fader: Fader) async -> String {
    let engine = stateLock.withLock { $0.engine }
    guard let params = await engine?.getProcessingParameters() else {
      return jsonReply("AdjustVolume", result: .processingNotRunningError)
    }
    var delta: Double = 0.0
    var minVol: Double = -150.0
    var maxVol: Double = 50.0
    if let val = arg as? Double {
      delta = val
    } else if let arr = arg as? [Any], arr.count >= 3,
      let d = arr[0] as? Double,
      let mn = arr[1] as? Double,
      let mx = arr[2] as? Double
    {
      delta = d
      minVol = mn
      maxVol = mx
    } else {
      return jsonReply(
        "AdjustVolume", result: .invalidRequestError("Invalid AdjustVolume argument"))
    }
    let current = params.targetVolume(for: fader)
    let newVol = min(maxVol, max(minVol, current + delta))
    params.setTargetVolume(newVol, for: fader)
    stateLock.withLock { $0.unsavedStateChanges = true }
    return jsonReply("AdjustVolume", result: .ok, value: "\(newVol)")
  }

  // MARK: - JSON Helpers

  private func jsonFromConfig(_ config: DSPConfiguration) throws -> [String: Any] {
    let data = try JSONEncoder().encode(config)
    guard let dict = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
      throw AudioBackendError.configParse(message: "Failed to build JSON dictionary from config")
    }
    return dict
  }

  private func faderForIndex(_ idx: Int) -> Fader? {
    switch idx {
    case 0: return .main
    case 1: return .aux1
    case 2: return .aux2
    case 3: return .aux3
    case 4: return .aux4
    default: return nil
    }
  }

  private func mergeJSON(_ target: inout [String: Any], patch: [String: Any]) {
    for (key, value) in patch {
      if let patchDict = value as? [String: Any],
        var targetDict = target[key] as? [String: Any]
      {
        mergeJSON(&targetDict, patch: patchDict)
        target[key] = targetDict
      } else {
        target[key] = value
      }
    }
  }

  private func getValueAtPointer(_ json: [String: Any], pointer: String) -> Any? {
    let components = pointer.split(separator: "/").map(String.init).filter { !$0.isEmpty }
    guard !components.isEmpty else { return nil }

    var current: Any = json
    for comp in components {
      if let dict = current as? [String: Any], let next = dict[comp] {
        current = next
      } else if let arr = current as? [Any], let idx = Int(comp), idx >= 0, idx < arr.count {
        current = arr[idx]
      } else {
        return nil
      }
    }
    return current
  }

  private func setValueAtPointer(_ json: inout [String: Any], pointer: String, value: Any) -> Bool {
    let components = pointer.split(separator: "/").map(String.init).filter { !$0.isEmpty }
    guard !components.isEmpty else { return false }

    if components.count == 1 {
      json[components[0]] = value
      return true
    }

    guard var nested = json[components[0]] as? [String: Any] else { return false }
    let subPointer = "/" + components.dropFirst().joined(separator: "/")
    if setValueAtPointer(&nested, pointer: subPointer, value: value) {
      json[components[0]] = nested
      return true
    }
    return false
  }
}

public final class ActiveConfigPath: Sendable {
  private let lock = OSAllocatedUnfairLock(initialState: String?.none)

  public init(initialPath: String? = nil) {
    self.lock.withLock { $0 = initialPath }
  }

  public var value: String? {
    get { lock.withLock { $0 } }
    set { lock.withLock { $0 = newValue } }
  }
}
