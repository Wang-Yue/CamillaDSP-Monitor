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

/// Pushed VU level event from SubscribeVuLevels.
public struct VuLevels: Sendable {
  public let playback_rms: [Float]
  public let playback_peak: [Float]
  public let capture_rms: [Float]
  public let capture_peak: [Float]
}

/// Pushed state change event from SubscribeState.
public struct StateUpdate: Sendable {
  public let state: String
  public let stopReason: String?
  public let stopReasonRate: Int?
}

public struct AudioDevice: Identifiable, Sendable {
  public var id: String { name }
  public let name: String
}

// MARK: - Device Capabilities (from GetCaptureDeviceCapabilities / GetPlaybackDeviceCapabilities)

public enum CapabilityMode: String, Codable, Sendable {
  case unified = "Unified"
  case shared = "Shared"
  case exclusive = "Exclusive"
}

public struct SamplerateCapability: Codable, Sendable {
  public let samplerate: Int
  public let formats: [String]
}

public struct ChannelCapability: Codable, Sendable {
  public let channels: Int
  public let samplerates: [SamplerateCapability]
}

public struct DeviceCapabilitySet: Codable, Sendable {
  public let mode: CapabilityMode
  public let capabilities: [ChannelCapability]
}

public struct AudioDeviceDescriptor: Codable, Sendable {
  public let name: String
  public let description: String
  public let capability_sets: [DeviceCapabilitySet]

  /// Supported sample rates for a given channel count.
  /// Falls back to the union across all channel counts if the count is not found.
  public func sampleRates(forChannels channels: Int) -> [Int] {
    guard let set = capability_sets.first else { return [] }
    let cap = set.capabilities.first(where: { $0.channels == channels }) ?? set.capabilities.first
    let rates: [Int]
    if let cap = cap {
      rates = cap.samplerates.map(\.samplerate)
    } else {
      rates = set.capabilities.flatMap { $0.samplerates.map(\.samplerate) }
    }
    return Set(rates).sorted()
  }

  /// Available sample formats for a given channel count and sample rate, sorted best-first.
  /// Preference order: S32 > S24 > S16 > F32 > F64.
  public func availableFormats(channels: Int, sampleRate: Int) -> [String] {
    guard let set = capability_sets.first else { return [] }
    let cap = set.capabilities.first(where: { $0.channels == channels }) ?? set.capabilities.first
    let formats = cap?.samplerates.first(where: { $0.samplerate == sampleRate })?.formats ?? []
    return formats.sorted { (Self.formatPriority[$0] ?? -1) > (Self.formatPriority[$1] ?? -1) }
  }

  /// Best sample format for a given channel count and sample rate.
  public func bestFormat(channels: Int, sampleRate: Int) -> String {
    availableFormats(channels: channels, sampleRate: sampleRate).first ?? "F32"
  }

  /// Channel counts this device supports, sorted ascending.
  public func availableChannels() -> [Int] {
    capability_sets.first?.capabilities.map(\.channels).sorted() ?? []
  }

  private static let formatPriority: [String: Int] = ["S32": 4, "S24": 3, "S16": 2, "F32": 1, "F64": 0]
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

  /// Kill any stale camilladsp processes. Safe to call from any context (synchronous).
  public static func killStaleCamillaDSP() {
    let task = Process()
    task.launchPath = "/usr/bin/env"
    task.arguments = ["pkill", "camilladsp"]
    try? task.run()
    task.waitUntilExit()
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
      // Wait up to 10 seconds for an in-progress connection attempt to complete
      var waitAttempts = 0
      while isConnecting && waitAttempts < 100 {
        try? await Task.sleep(nanoseconds: 100_000_000)
        waitAttempts += 1
      }
      if isConnected { return }
      if isConnecting {
        // Timed out waiting for previous connection — force reset and try fresh
        isConnecting = false
      }
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

    DSPEngine.killStaleCamillaDSP()

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
      // Don't call waitUntilExit() synchronously — it blocks the actor's executor.
      // The process will be cleaned up by the OS after termination.
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

  public func fetchProcessingLoad() async -> Float {
    let pLoad = await fetchLoad("GetProcessingLoad")
    let rLoad = await fetchLoad("GetResamplerLoad")
    return pLoad + rLoad
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

  /// Fetches the sample rates and formats supported by a specific device via
  /// GetCaptureDeviceCapabilities / GetPlaybackDeviceCapabilities.
  public func getDeviceCapabilities(
    backend: String, device: String, isCapture: Bool
  ) async -> AudioDeviceDescriptor? {
    let cmd = isCapture ? "GetCaptureDeviceCapabilities" : "GetPlaybackDeviceCapabilities"
    let params: [any Sendable] = [backend, device]
    do {
      guard let valueDict: [String: any Sendable] = try await sendCommand(cmd, value: params)
      else { return nil }
      let data = try JSONSerialization.data(withJSONObject: valueDict)
      return try JSONDecoder().decode(AudioDeviceDescriptor.self, from: data)
    } catch {
      print("[DSPEngine] \(cmd) failed: \(error)")
      return nil
    }
  }

  // MARK: - VU Level Subscription

  /// Opens a separate WebSocket connection and subscribes to VU level events.
  /// Returns an AsyncStream that yields VuLevels pushed by CamillaDSP.
  /// Returns nil if the server doesn't support subscriptions.
  /// The stream ends when the connection closes or an error occurs.
  public func subscribeVuLevels(
    maxRate: Float = 10.0, attack: Float = 5.0, release: Float = 100.0
  ) async -> AsyncStream<VuLevels>? {
    guard isConnected else { return nil }

    // Open a dedicated WebSocket for this subscription
    let subWs = session.webSocketTask(with: url)
    subWs.resume()

    // Send SubscribeVuLevels command
    let params: [String: Any] = ["max_rate": maxRate, "attack": attack, "release": release]
    let cmd: [String: Any] = ["SubscribeVuLevels": params]
    guard let data = try? JSONSerialization.data(withJSONObject: cmd),
      let jsonString = String(data: data, encoding: .utf8)
    else {
      subWs.cancel(with: .goingAway, reason: nil)
      return nil
    }

    do {
      try await subWs.send(.string(jsonString))
      let response = try await subWs.receive()

      // Check if subscription was accepted
      let responseData: Data
      switch response {
      case .string(let s): responseData = s.data(using: .utf8)!
      case .data(let d): responseData = d
      @unknown default:
        subWs.cancel(with: .goingAway, reason: nil)
        return nil
      }

      guard let dict = try? JSONSerialization.jsonObject(with: responseData) as? [String: Any],
        let reply = dict["SubscribeVuLevels"] as? [String: Any],
        let result = reply["result"] as? String, result == "Ok"
      else {
        // Server doesn't support subscriptions
        print("[DSPEngine] SubscribeVuLevels not supported, falling back to polling")
        subWs.cancel(with: .goingAway, reason: nil)
        return nil
      }
    } catch {
      subWs.cancel(with: .goingAway, reason: nil)
      return nil
    }

    print(
      "[DSPEngine] VU subscription active (rate=\(maxRate) attack=\(attack) release=\(release))")

    // Return a stream that reads pushed VuLevelsEvent messages
    return AsyncStream { continuation in
      continuation.onTermination = { _ in
        // Try to send StopSubscription before closing
        Task {
          try? await subWs.send(.string("\"StopSubscription\""))
          subWs.cancel(with: .goingAway, reason: nil)
        }
      }

      Task.detached {
        while true {
          do {
            let msg = try await subWs.receive()
            let msgData: Data
            switch msg {
            case .string(let s): msgData = s.data(using: .utf8) ?? Data()
            case .data(let d): msgData = d
            @unknown default: continue
            }

            guard
              let dict = try? JSONSerialization.jsonObject(with: msgData) as? [String: Any],
              let event = dict["VuLevelsEvent"] as? [String: Any],
              let value = event["value"] as? [String: Any],
              let pbRms = value["playback_rms"] as? [Double],
              let pbPeak = value["playback_peak"] as? [Double],
              let capRms = value["capture_rms"] as? [Double],
              let capPeak = value["capture_peak"] as? [Double]
            else { continue }

            continuation.yield(
              VuLevels(
                playback_rms: pbRms.map { Float($0) },
                playback_peak: pbPeak.map { Float($0) },
                capture_rms: capRms.map { Float($0) },
                capture_peak: capPeak.map { Float($0) }
              ))
          } catch {
            let nsError = error as NSError
            // Suppress expected disconnection/cancellation errors
            if nsError.code != -999 && nsError.code != 57 {
              print("[DSPEngine] VU subscription ended: \(error)")
            }
            continuation.finish()
            return
          }
        }
      }
    }
  }

  // MARK: - State Subscription

  /// Opens a separate WebSocket connection and subscribes to state change events.
  /// Returns an AsyncStream that yields StateUpdate pushed by CamillaDSP.
  /// Returns nil if the server doesn't support subscriptions.
  public func subscribeState() async -> AsyncStream<StateUpdate>? {
    guard isConnected else { return nil }

    let subWs = session.webSocketTask(with: url)
    subWs.resume()

    do {
      try await subWs.send(.string("\"SubscribeState\""))
      let response = try await subWs.receive()

      let responseData: Data
      switch response {
      case .string(let s): responseData = s.data(using: .utf8)!
      case .data(let d): responseData = d
      @unknown default:
        subWs.cancel(with: .goingAway, reason: nil)
        return nil
      }

      guard let dict = try? JSONSerialization.jsonObject(with: responseData) as? [String: Any],
        let reply = dict["SubscribeState"] as? [String: Any],
        let result = reply["result"] as? String, result == "Ok"
      else {
        print("[DSPEngine] SubscribeState not supported, falling back to polling")
        subWs.cancel(with: .goingAway, reason: nil)
        return nil
      }
    } catch {
      subWs.cancel(with: .goingAway, reason: nil)
      return nil
    }

    print("[DSPEngine] State subscription active")

    return AsyncStream { continuation in
      continuation.onTermination = { _ in
        Task {
          try? await subWs.send(.string("\"StopSubscription\""))
          subWs.cancel(with: .goingAway, reason: nil)
        }
      }

      Task.detached {
        while true {
          do {
            let msg = try await subWs.receive()
            let msgData: Data
            switch msg {
            case .string(let s): msgData = s.data(using: .utf8) ?? Data()
            case .data(let d): msgData = d
            @unknown default: continue
            }

            guard
              let dict = try? JSONSerialization.jsonObject(with: msgData) as? [String: Any],
              let event = dict["StateEvent"] as? [String: Any],
              let value = event["value"] as? [String: Any],
              let state = value["state"] as? String
            else { continue }

            // stop_reason can be a String (e.g. "None") or a Dict (e.g. {"CaptureFormatChange": 44100})
            let stopReason: String?
            var stopReasonRate: Int? = nil
            if let reasonStr = value["stop_reason"] as? String {
              stopReason = reasonStr
            } else if let reasonDict = value["stop_reason"] as? [String: Any] {
              stopReason = reasonDict.keys.first
              if stopReason == "CaptureFormatChange" {
                if let rate = reasonDict[stopReason!] as? Int {
                  stopReasonRate = rate
                } else if let rate = reasonDict[stopReason!] as? Double {
                  stopReasonRate = Int(rate)
                }
              }
            } else {
              stopReason = nil
            }
            continuation.yield(
              StateUpdate(state: state, stopReason: stopReason, stopReasonRate: stopReasonRate))
          } catch {
            let nsError = error as NSError
            if nsError.code != -999 && nsError.code != 57 {
              print("[DSPEngine] State subscription ended: \(error)")
            }
            continuation.finish()
            return
          }
        }
      }
    }
  }
}
