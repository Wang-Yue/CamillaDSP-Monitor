import Foundation

public typealias PrcFmt = Double

extension PrcFmt {
  public static func toDB(_ linear: Float) -> Double {
    linear <= 0 ? -100.0 : 20.0 * log10(Double(linear))
  }
}

public enum AudioBackendError: Error, LocalizedError, Sendable {
  case commandFailed(String)
  case connectionFailed(String)
  case notConnected
  case binaryNotFound
  case decodingError(String)

  public var errorDescription: String? {
    switch self {
    case .commandFailed(let msg): return "CamillaDSP command failed: \(msg)"
    case .connectionFailed(let msg): return "Could not connect to CamillaDSP: \(msg)"
    case .notConnected: return "Not connected to CamillaDSP"
    case .binaryNotFound: return "CamillaDSP binary not found"
    case .decodingError(let msg): return "Failed to decode response: \(msg)"
    }
  }
}

public struct SignalLevels: Codable, Sendable {
  public var processing_load: Float?
  public let capture_rms: [Float]
  public let capture_peak: [Float]
  public let playback_rms: [Float]
  public let playback_peak: [Float]
}

public struct AudioDevice: Identifiable, Sendable {
  public var id: String { name }
  public let name: String
}

public actor DSPEngine {
  private let url: URL
  private var webSocket: URLSessionWebSocketTask?
  private let session: URLSession
  private var isConnected = false
  private var isConnecting = false
  private var process: Process?

  public init(host: String = "127.0.0.1", port: Int = 1234) {
    self.url = URL(string: "ws://\(host):\(port)")!
    self.session = URLSession(configuration: .default)
  }

  deinit {
    if let p = process, p.isRunning {
      p.terminate()
      p.waitUntilExit()
    }
  }

  // MARK: - Lifecycle

  public func connect(binaryPath: String) async throws {
    if isConnected { return }
    if isConnecting {
      while isConnecting { try? await Task.sleep(nanoseconds: 100_000_000) }
      if isConnected { return }
    }

    isConnecting = true
    defer { isConnecting = false }

    if process == nil || !process!.isRunning {
      try startProcess(binaryPath: binaryPath)
    }

    let maxAttempts = 20
    for attempt in 1...maxAttempts {
      let ws = session.webSocketTask(with: url)
      ws.resume()

      do {
        // Send a ping to verify connection
        let message = URLSessionWebSocketTask.Message.string("\"GetVersion\"")
        try await ws.send(message)
        _ = try await ws.receive()

        webSocket = ws
        isConnected = true
        print("[DSPEngine] Connected on attempt \(attempt)")
        return
      } catch {
        ws.cancel(with: .goingAway, reason: nil)
        if attempt == maxAttempts {
          throw AudioBackendError.connectionFailed(error.localizedDescription)
        }
        try? await Task.sleep(nanoseconds: 250_000_000)
      }
    }
  }

  private func startProcess(binaryPath: String) throws {
    guard !binaryPath.isEmpty && FileManager.default.fileExists(atPath: binaryPath) else {
      throw AudioBackendError.binaryNotFound
    }

    let killTask = Process()
    killTask.launchPath = "/usr/bin/env"
    killTask.arguments = ["pkill", "camilladsp"]
    try? killTask.run()
    killTask.waitUntilExit()

    print("[DSPEngine] Launching \(binaryPath)... ")
    let p = Process()
    p.executableURL = URL(fileURLWithPath: binaryPath)
    p.arguments = ["-p", "1234", "-w"]

    let outPipe = Pipe()
    let errPipe = Pipe()
    p.standardOutput = outPipe
    p.standardError = errPipe

    outPipe.fileHandleForReading.readabilityHandler = { handle in
      let data = handle.availableData
      if let str = String(data: data, encoding: .utf8), !str.isEmpty {
        print("[CamillaDSP] \(str)", terminator: "")
      }
    }
    errPipe.fileHandleForReading.readabilityHandler = { handle in
      let data = handle.availableData
      if let str = String(data: data, encoding: .utf8), !str.isEmpty {
        print("[CamillaDSP ERR] \(str)", terminator: "")
      }
    }

    try p.run()
    self.process = p
  }

  public func disconnect() {
    webSocket?.cancel(with: .goingAway, reason: nil)
    webSocket = nil
    isConnected = false
    if let p = process, p.isRunning {
      p.terminate()
      p.waitUntilExit()
    }
    process = nil
  }

  // MARK: - Messaging

  public func sendCommand<T: Sendable>(_ type: String, value: (any Sendable)? = nil) async throws
    -> T?
  {
    guard isConnected, let webSocket = webSocket else {
      throw AudioBackendError.notConnected
    }

    let jsonString: String
    if let val = value {
      let dict: [String: any Sendable] = [type: val]
      let data = try JSONSerialization.data(withJSONObject: dict)
      jsonString = String(data: data, encoding: .utf8)!
    } else {
      jsonString = "\"\(type)\""
    }

    let isPolling = ["GetSignalLevels", "GetProcessingLoad", "GetResamplerLoad", "GetState"]
      .contains(type)
    if !isPolling {
      print("[DSPEngine] SEND: \(jsonString)")
    }

    do {
      try await webSocket.send(.string(jsonString))
      let response = try await webSocket.receive()

      let responseData: Data
      switch response {
      case .data(let data): responseData = data
      case .string(let string): responseData = string.data(using: .utf8)!
      @unknown default: throw AudioBackendError.commandFailed("Unknown websocket message type")
      }

      let responseDict =
        try JSONSerialization.jsonObject(with: responseData) as? [String: any Sendable]

      if let invalid = responseDict?["Invalid"] as? [String: any Sendable],
        let errorMsg = invalid["error"] as? String
      {
        print("[DSPEngine] INVALID COMMAND: \(errorMsg)")
        throw AudioBackendError.commandFailed(errorMsg)
      }

      if let cmdResult = responseDict?[type] as? [String: any Sendable] {
        if let result = cmdResult["result"] {
          if let resultStr = result as? String {
            if resultStr != "Ok" {
              throw AudioBackendError.commandFailed(resultStr)
            }
          } else if let resultDict = result as? [String: any Sendable] {
            let errorType = resultDict.keys.first ?? "Unknown"
            let errorVal = resultDict.values.first
            let errorMsg = "\(errorVal ?? "No details")"
            print("[DSPEngine] ERROR (\(type)): \(errorType): \(errorMsg)")
            throw AudioBackendError.commandFailed("\(errorType): \(errorMsg)")
          }
        }
        return cmdResult["value"] as? T
      }

      return nil
    } catch {
      if error is AudioBackendError { throw error }

      isConnected = false
      webSocket.cancel(with: .goingAway, reason: nil)
      throw error
    }
  }

  // MARK: - Commands

  public func ping() async -> Bool {
    guard isConnected else { return false }
    do {
      let _: String? = try await sendCommand("GetVersion")
      return true
    } catch {
      return false
    }
  }

  public func start(configJson: String) async throws {
    let _: String? = try await sendCommand("SetConfigJson", value: configJson)
  }

  public func stop() async {
    let _: String? = try? await sendCommand("Stop")
  }

  public func setVolume(_ db: Double) async {
    let _: String? = try? await sendCommand("SetVolume", value: Float(db))
  }

  public func setMute(_ mute: Bool) async {
    let _: String? = try? await sendCommand("SetMute", value: mute)
  }

  public func setFaderExternalVolume(fader: Int, db: Double) async {
    let value: [any Sendable] = [fader, Float(db)]
    let _: String? = try? await sendCommand("SetFaderExternalVolume", value: value)
  }

  public func setFaderMute(fader: Int, mute: Bool) async {
    let value: [any Sendable] = [fader, mute]
    let _: String? = try? await sendCommand("SetFaderMute", value: value)
  }

  public func getSignalLevels() async -> SignalLevels? {
    do {
      guard let levelsValue: [String: any Sendable] = try await sendCommand("GetSignalLevels")
      else { return nil }
      let data = try JSONSerialization.data(withJSONObject: levelsValue)
      var levels = try JSONDecoder().decode(SignalLevels.self, from: data)

      let pLoad = await fetchLoad("GetProcessingLoad")
      let rLoad = await fetchLoad("GetResamplerLoad")
      levels.processing_load = pLoad + rLoad
      return levels
    } catch {
      return nil
    }
  }

  private func fetchLoad(_ command: String) async -> Float {
    do {
      guard let loadValue: any Sendable = try await sendCommand(command) else { return 0.0 }
      if let d = loadValue as? Double { return Float(d) }
      if let f = loadValue as? Float { return f }
      if let i = loadValue as? Int { return Float(i) }
    } catch {
    }
    return 0.0
  }

  public func getAvailableDevices(backend: String, input: Bool) async -> [AudioDevice] {
    let cmd = input ? "GetAvailableCaptureDevices" : "GetAvailablePlaybackDevices"
    do {
      guard let value: [[String]] = try await sendCommand(cmd, value: backend) else { return [] }
      return value.map { AudioDevice(name: $0[0]) }
    } catch {
      return []
    }
  }
}
