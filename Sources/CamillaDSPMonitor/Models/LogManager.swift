import Foundation
import SwiftUI

struct LogEntry: Identifiable {
  let id = UUID()
  let timestamp = Date()
  let message: String
}

@MainActor
class LogManager: ObservableObject {
  @Published var entries: [LogEntry] = []
  private let maxEntries = 2000

  private let outPipe = Pipe()
  private let errPipe = Pipe()
  private var leftoverData = Data()

  init() {
    setupCapture()
  }

  private func setupCapture() {
    // Disable buffering for stdout and stderr so they flush immediately
    setvbuf(stdout, nil, _IOLBF, 0)
    setvbuf(stderr, nil, _IOLBF, 0)

    let outHandle = outPipe.fileHandleForWriting
    let errHandle = errPipe.fileHandleForWriting

    // Redirect stdout and stderr to our pipes
    dup2(outHandle.fileDescriptor, STDOUT_FILENO)
    dup2(errHandle.fileDescriptor, STDERR_FILENO)

    outPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
      let data = handle.availableData
      Task { @MainActor in
        self?.processData(data)
      }
    }

    errPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
      let data = handle.availableData
      Task { @MainActor in
        self?.processData(data)
      }
    }
  }

  private func processData(_ data: Data) {
    guard !data.isEmpty else { return }
    
    // Combine with leftover data from previous read
    var combinedData = leftoverData
    combinedData.append(data)
    
    guard let str = String(data: combinedData, encoding: .utf8) else {
      // If data is not valid UTF-8, it might be a split character at the end.
      // Keep it and try again next time.
      leftoverData = combinedData
      return
    }
    
    let lines = str.components(separatedBy: .newlines)
    
    // If the string does not end with a newline, the last component is incomplete.
    if !str.hasSuffix("\n") {
      leftoverData = lines.last?.data(using: .utf8) ?? Data()
    } else {
      leftoverData = Data()
    }
    
    // Process all complete lines
    let completeLines = str.hasSuffix("\n") ? lines : lines.dropLast()
    
    for line in completeLines where !line.isEmpty {
      self.entries.append(LogEntry(message: line))
    }
    
    if self.entries.count > self.maxEntries {
      self.entries.removeFirst(self.entries.count - self.maxEntries)
    }
  }
}
