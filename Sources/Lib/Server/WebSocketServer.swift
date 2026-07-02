// CamillaDSP-Swift: WebSocket control server
// Provides runtime control API compatible with CamillaDSP's protocol

import DSPAudio
import DSPConfig
import DSPEngine
import DSPLogging
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

private func jsonArray(_ values: [PrcFmt]) -> String {
  return "[\(values.map { String($0) }.joined(separator: ","))]"
}

public final class WebSocketServer: Sendable {
  private let logger = Logger(label: "dsp.websocket")
  private let port: UInt16
  private let host: String
  private let activePath: ActiveConfigPath

  // Connection management state protected by a lock
  private let stateLock = OSAllocatedUnfairLock(initialState: State())

  private struct ConnectionSubscription: Sendable {
    var stateSubscribed: Bool = false
    var vuSubscribed: Bool = false
    var lastState: String = ""
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
  }

  public init(port: UInt16, host: String = "127.0.0.1", activePath: ActiveConfigPath) {
    self.port = port
    self.host = host
    self.activePath = activePath
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
          try? await Task.sleep(nanoseconds: 100_000_000)  // 100ms
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
    let vu = await engine.getVuLevels()

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

    let vuData = try? JSONEncoder().encode(vu)
    let vuValueJSON = vuData.flatMap { String(data: $0, encoding: .utf8) } ?? "{}"

    let connectionsToNotify = stateLock.withLock { state -> [(NWConnection, String)] in
      var list: [(NWConnection, String)] = []
      for conn in state.connections {
        let id = ObjectIdentifier(conn)
        var sub = state.subscriptions[id] ?? ConnectionSubscription()

        if sub.stateSubscribed && sub.lastState != stateStr {
          sub.lastState = stateStr
          state.subscriptions[id] = sub
          let msg = "{\"StateEvent\":{\"result\":\"Ok\",\"value\":\(stateValueJSON)}}"
          list.append((conn, msg))
        }

        if sub.vuSubscribed {
          let msg = "{\"VuLevelsEvent\":{\"result\":\"Ok\",\"value\":\(vuValueJSON)}}"
          list.append((conn, msg))
        }
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
      return jsonReply("GetRateAdjust", result: .ok, value: "1.0")

    case "GetBufferLevel":
      return jsonReply("GetBufferLevel", result: .ok, value: "0")

    case "GetClippedSamples":
      return jsonReply("GetClippedSamples", result: .ok, value: "0")

    case "ResetClippedSamples":
      return jsonReply("ResetClippedSamples", result: .ok)

    case "GetProcessingLoad":
      return jsonReply("GetProcessingLoad", result: .ok, value: "0.0")

    case "GetResamplerLoad":
      return jsonReply("GetResamplerLoad", result: .ok, value: "0.0")

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
        state.subscriptions[id] = sub
      }
      return jsonReply("SubscribeVuLevels", result: .ok)

    case "StopSubscription":
      let found = stateLock.withLock { state in
        let id = ObjectIdentifier(connection)
        if state.subscriptions[id] != nil {
          state.subscriptions.removeValue(forKey: id)
          return true
        }
        return false
      }
      if found {
        return jsonReply("StopSubscription", result: .ok)
      } else {
        return jsonReply(
          "StopSubscription", result: .invalidRequestError("No active subscription to stop"))
      }

    default:
      // Try JSON object commands
      return await handleJSONCommand(jsonText: trimmed)
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

  private func handleJSONCommand(jsonText: String) async -> String {
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
