import Foundation

public typealias PrcFmt = Double

extension PrcFmt {
  public static func toDB(_ linear: Float) -> Double {
    linear <= 0 ? -100.0 : 20.0 * log10(Double(linear))
  }
}

public enum EngineState: String, Codable, Sendable {
  case inactive = "Inactive"
  case running = "Running"
}

public enum AudioBackendError: Error, Sendable {
  case commandFailed(String)
  case connectionFailed(String)
  case notConnected
  case binaryNotFound
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

  public func connect(binaryPath: String) async throws {
    if isConnected { return }

    if process == nil || !process!.isRunning {
      try startProcess(binaryPath: binaryPath)
    }

    let maxAttempts = 20
    for attempt in 1...maxAttempts {
      let ws = session.webSocketTask(with: url)
      ws.resume()

      do {
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
          print("[DSPEngine] Connection failed after \(maxAttempts) attempts: \(error)")
          throw AudioBackendError.connectionFailed(error.localizedDescription)
        }
        try? await Task.sleep(nanoseconds: 250_000_000)
      }
    }
  }

  private func startProcess(binaryPath: String) throws {
    guard !binaryPath.isEmpty && FileManager.default.fileExists(atPath: binaryPath) else {
      print("[DSPEngine] ERROR: camilladsp binary not found at \(binaryPath)")
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

    let pipe = Pipe()
    p.standardOutput = pipe
    p.standardError = pipe

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

    if type != "GetSignalLevels" && type != "GetProcessingLoad" && type != "GetState"
      && type != "GetStopReason"
    {
      print("[DSPEngine] SEND: \(jsonString)")
    }

    let message = URLSessionWebSocketTask.Message.string(jsonString)
    try await webSocket.send(message)

    let response = try await webSocket.receive()

    let responseData: Data
    switch response {
    case .data(let data):
      responseData = data
    case .string(let string):
      responseData = string.data(using: .utf8)!
    @unknown default:
      throw AudioBackendError.commandFailed("Unknown response type")
    }

    let responseDict =
      try JSONSerialization.jsonObject(with: responseData) as? [String: any Sendable]

    if let resultContainer = responseDict?[type] as? [String: any Sendable] {
      if let result = resultContainer["result"] as? String {
        if result != "Ok" {
          print(
            "[DSPEngine] RECV ERROR for \(type): \(String(data: responseData, encoding: .utf8) ?? "")"
          )
          throw AudioBackendError.commandFailed(result)
        }
      }
      return resultContainer["value"] as? T
    } else if let result = responseDict?["result"] as? String {
      if result == "Error" {
        print(
          "[DSPEngine] RECV ERROR for \(type): \(String(data: responseData, encoding: .utf8) ?? "")"
        )
        throw AudioBackendError.commandFailed(
          responseDict?["message"] as? String ?? "Unknown error")
      }
      return responseDict?["value"] as? T
    }

    return nil
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

  public func getSignalLevels() async -> SignalLevels? {
    // Return [String: any Sendable] to avoid type mismatch with Floats from JSON
    guard let levelsValue: [String: any Sendable] = try? await sendCommand("GetSignalLevels"),
      let data = try? JSONSerialization.data(withJSONObject: levelsValue)
    else { return nil }

    var levels = try? JSONDecoder().decode(SignalLevels.self, from: data)

    if levels != nil {
      if let loadValue: Double = try? await sendCommand("GetProcessingLoad") {
        levels?.processing_load = Float(loadValue)
      }
    }
    return levels
  }

  public func getAvailableDevices(backend: String, input: Bool) async -> [AudioDevice] {
    let cmd = input ? "GetAvailableCaptureDevices" : "GetAvailablePlaybackDevices"
    guard let value: [[String]] = try? await sendCommand(cmd, value: backend) else { return [] }
    return value.map { AudioDevice(name: $0[0]) }
  }
}
