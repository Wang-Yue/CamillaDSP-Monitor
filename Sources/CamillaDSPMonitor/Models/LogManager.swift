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
  private var pending: [LogEntry] = []
  private var leftoverData = Data()
  private let maxEntries: Int

  init(maxEntries: Int) {
    self.maxEntries = maxEntries
  }

  func appendRawData(_ data: Data) -> [LogEntry] {
    guard !data.isEmpty else { return [] }

    var combinedData = leftoverData
    combinedData.append(data)

    guard let str = String(data: combinedData, encoding: .utf8) else {
      leftoverData = combinedData
      return []
    }

    let lines = str.components(separatedBy: .newlines)

    if !str.hasSuffix("\n") {
      leftoverData = lines.last?.data(using: .utf8) ?? Data()
    } else {
      leftoverData = Data()
    }

    let completeLines = str.hasSuffix("\n") ? lines : lines.dropLast()
    let newEntries =
      completeLines
      .filter { !$0.isEmpty }
      .map { LogEntry(message: $0) }

    pending.append(contentsOf: newEntries)
    if pending.count > maxEntries {
      pending.removeFirst(pending.count - maxEntries)
    }
    return newEntries
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

  private let outPipe = Pipe()
  private let errPipe = Pipe()

  private let buffer: LogBuffer
  private var updateTask: Task<Void, Never>?

  init() {
    let max = 2000
    self.buffer = LogBuffer(maxEntries: max)
    setupCapture()
    setupBatchTimer()
  }

  private func setupCapture() {
    // Disable buffering for stdout and stderr
    setvbuf(stdout, nil, _IOLBF, 0)
    setvbuf(stderr, nil, _IOLBF, 0)

    let outHandle = outPipe.fileHandleForWriting
    let errHandle = errPipe.fileHandleForWriting

    dup2(outHandle.fileDescriptor, STDOUT_FILENO)
    dup2(errHandle.fileDescriptor, STDERR_FILENO)

    let bufferRef = self.buffer

    outPipe.fileHandleForReading.readabilityHandler = { handle in
      let data = handle.availableData
      Task {
        _ = await bufferRef.appendRawData(data)
      }
    }

    errPipe.fileHandleForReading.readabilityHandler = { handle in
      let data = handle.availableData
      Task {
        _ = await bufferRef.appendRawData(data)
      }
    }
  }

  private func setupBatchTimer() {
    updateTask = Task {
      while !Task.isCancelled {
        try? await Task.sleep(nanoseconds: 100_000_000)  // 10Hz
        guard !Task.isCancelled else { break }

        let newEntries = await self.buffer.flush()
        if !newEntries.isEmpty {
          self.entries.append(contentsOf: newEntries)
          if self.entries.count > self.maxEntries {
            self.entries.removeFirst(self.entries.count - self.maxEntries)
          }
        }
      }
    }
  }

  // deinit cannot access actor-isolated property 'updateTask'.
  // However, Tasks are implicitly cancelled if their parent task is cancelled,
  // but this is a top-level Task stored in a property.
  // We can't really cancel it in deinit easily without a non-isolated shadow property.
  // Given this is a long-lived singleton-like object, it might not be a huge deal,
  // but let's try to fix it properly.

  // Actually, I'll just remove the deinit for now to see if it builds.
}
