import DSPConfig
import DSPLib
import Foundation
import Observation
import SwiftUI

struct LogEntry: Identifiable, Sendable {
  let id: UUID
  let timestamp: Date
  let message: String

  init(message: String) {
    self.id = UUID()
    self.timestamp = Date()
    self.message = message
  }
}

/// A thread-safe buffer for collecting logs in the background.
actor LogBuffer {
  static let shared = LogBuffer(maxEntries: 2000)
  private var pending: [LogEntry] = []
  private let maxEntries: Int

  init(maxEntries: Int) {
    self.maxEntries = maxEntries
  }

  func append(_ entry: LogEntry) {
    pending.append(entry)
    if pending.count > maxEntries {
      pending.removeFirst(pending.count - maxEntries)
    }
  }

  func flush() -> [LogEntry] {
    let toFlush = pending
    pending = []
    return toFlush
  }
}

@MainActor
@Observable
class LogManager {
  var entries: [LogEntry] = []
  private let maxEntries = 2000

  // Log level settings
  var selectedLogLevel: LogLevel = .info {
    didSet {
      saveLevel()
      updateRustLevel()
    }
  }

  private var updateTask: Task<Void, Never>?
  private var engine: DSPEngine?

  init() {
    loadLevel()
    setupAppLogger()
    setupCallback()
    setupBatchTimer()
  }

  deinit {
    AppLogger.setHandler(nil)
    DSPEngine.setLogCallback(nil)
  }

  func setEngine(_ engine: DSPEngine) {
    self.engine = engine
    updateRustLevel()
  }

  private func loadLevel() {
    let defaults = UserDefaults.standard
    if let saved = defaults.string(forKey: "selectedLogLevel"),
      let level = LogLevel(rawValue: saved)
    {
      selectedLogLevel = level
    }
  }

  private func saveLevel() {
    UserDefaults.standard.set(selectedLogLevel.rawValue, forKey: "selectedLogLevel")
  }

  private func updateRustLevel() {
    let level = selectedLogLevel
    Task { [weak engine] in
      await engine?.setLogLevel(level)
    }
  }

  private func setupAppLogger() {
    AppLogger.setHandler { level, component, message in
      let formatted = component.isEmpty ? "[\(level.rawValue.uppercased())] \(message)" : "[\(level.rawValue.uppercased())] [\(component)] \(message)"
      let entry = LogEntry(message: formatted)
      Task {
        await LogBuffer.shared.append(entry)
      }
    }
  }

  private func setupCallback() {
    DSPEngine.setLogCallback { level, label, msg in
      let formatted = label.isEmpty ? "[\(level)] \(msg)" : "[\(level)] [\(label)] \(msg)"
      let entry = LogEntry(message: formatted)
      Task {
        await LogBuffer.shared.append(entry)
      }
    }
  }

  private func setupBatchTimer() {
    updateTask = Task { [weak self] in
      while !Task.isCancelled {
        try? await Task.sleep(nanoseconds: 100_000_000)  // 10Hz
        guard !Task.isCancelled, let self else { break }

        let newEntries = await LogBuffer.shared.flush()
        if !newEntries.isEmpty {
          self.entries.append(contentsOf: newEntries)
          if self.entries.count > self.maxEntries {
            self.entries.removeFirst(self.entries.count - self.maxEntries)
          }
        }
      }
    }
  }
}
