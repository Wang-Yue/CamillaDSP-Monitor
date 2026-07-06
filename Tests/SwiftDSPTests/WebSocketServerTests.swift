import DSPEngine
import DSPServer
import Foundation
import Testing

@Suite struct WebSocketServerTests {
  @Test func TestWebSocketCommands() async throws {
    let engine = SwiftDSPEngine()
    let server = WebSocketServer(port: 54321, host: "127.0.0.1", activePath: ActiveConfigPath())
    server.setEngine(engine)

    try server.start()
    defer {
      server.stop()
    }

    // Give the server a small moment to start listening
    try await Task.sleep(nanoseconds: 50_000_000)

    let session = URLSession(configuration: .default)
    let url = URL(string: "ws://127.0.0.1:54321")!
    let wsTask = session.webSocketTask(with: url)
    wsTask.resume()

    // Send GetVersion command
    let command = "\"GetVersion\""
    try await wsTask.send(.string(command))

    // Receive response
    let response = try await wsTask.receive()
    if case .string(let text) = response {
      #expect(text.contains("\"GetVersion\""))
      #expect(text.contains("\"Ok\""))
      #expect(text.contains("\"CamillaDSP-Swift-Embedded 2.0.0\""))
    } else {
      Issue.record("Expected text message response")
    }

    // Send GetState command
    try await wsTask.send(.string("\"GetState\""))
    let stateResponse = try await wsTask.receive()
    if case .string(let text) = stateResponse {
      #expect(text.contains("\"GetState\""))
      #expect(text.contains("\"Ok\""))
      #expect(text.contains("\"Inactive\""))
    } else {
      Issue.record("Expected text message response")
    }

    wsTask.cancel(with: .normalClosure, reason: nil)
  }
}
