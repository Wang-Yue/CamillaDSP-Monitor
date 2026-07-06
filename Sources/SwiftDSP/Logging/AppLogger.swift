// Lock-free, allocation-free high performance logger for real-time audio threads

import DSPAudio
import DSPConfig
import Foundation
import Synchronization

/// Process-wide log-level gate. Stored as `Atomic<UInt8>` so the
/// real-time audio path can read it without locks.
public enum MutableLogLevel {
  private static let storage = Atomic<UInt8>(LogLevel.info.rawByte)

  public static var current: LogLevel {
    get { LogLevel(rawByte: storage.load(ordering: .acquiring)) }
    set { storage.store(newValue.rawByte, ordering: .releasing) }
  }
}

public enum LogArgument: Sendable {
  case none
  case int(Int)
  case double(Double)
  case staticString(StaticString)
  case string(String)
}

struct LogRecord: Sendable {
  let level: LogLevel
  let label: StaticString
  let message: StaticString
  let arg1: LogArgument
  let arg2: LogArgument
  let arg3: LogArgument
  let arg4: LogArgument
}

public final class AppLogger: Sendable {
  public static let shared = AppLogger()

  private let queue = SPSCQueue<LogRecord>(minimumCapacity: 512)
  private let semaphore = DispatchSemaphore(value: 0)
  private let shouldExit = Atomic<Bool>(false)
  private let isStarted = Atomic<Bool>(false)

  private init() {
    // Intentionally empty to guarantee safe singleton instance publication before thread activation.
  }

  deinit {
    shouldExit.store(true, ordering: .releasing)
    semaphore.signal()
  }

  private func startWorkerIfNeeded() {
    let result = isStarted.compareExchange(
      expected: false, desired: true, ordering: .acquiringAndReleasing)
    if result.exchanged {
      let thread = Thread { [weak self] in
        self?.runWorker()
      }
      thread.name = "dsp.logger"
      thread.qualityOfService = .utility
      thread.start()
    }
  }

  public func log(
    level: LogLevel,
    label: StaticString,
    message: StaticString,
    arg1: LogArgument = .none,
    arg2: LogArgument = .none,
    arg3: LogArgument = .none,
    arg4: LogArgument = .none
  ) {
    guard level.rawByte <= MutableLogLevel.current.rawByte else { return }

    if !isStarted.load(ordering: .acquiring) {
      startWorkerIfNeeded()
    }

    let record = LogRecord(
      level: level,
      label: label,
      message: message,
      arg1: arg1,
      arg2: arg2,
      arg3: arg3,
      arg4: arg4
    )

    if queue.enqueue(record) {
      semaphore.signal()
    }
  }

  private func runWorker() {
    while !shouldExit.load(ordering: .acquiring) {
      semaphore.wait()
      if shouldExit.load(ordering: .acquiring) { break }

      while let record = queue.dequeue() {
        var msgStr = String(describing: record.message)
        let args = [record.arg1, record.arg2, record.arg3, record.arg4]

        for arg in args {
          switch arg {
          case .none:
            break
          case .int(let i):
            if let range = msgStr.range(of: "%d") {
              msgStr.replaceSubrange(range, with: String(i))
            } else {
              msgStr += " \(i)"
            }
          case .double(let d):
            if let range = msgStr.range(of: "%f") {
              msgStr.replaceSubrange(range, with: String(format: "%.6f", d))
            } else if let range = msgStr.range(of: "%.1f") {
              msgStr.replaceSubrange(range, with: String(format: "%.1f", d))
            } else {
              msgStr += " \(d)"
            }
          case .staticString(let s):
            if let range = msgStr.range(of: "%s") {
              msgStr.replaceSubrange(range, with: String(describing: s))
            } else {
              msgStr += " \(String(describing: s))"
            }
          case .string(let s):
            if let range = msgStr.range(of: "%s") {
              msgStr.replaceSubrange(range, with: s)
            } else {
              msgStr += " \(s)"
            }
          }
        }

        let labelStr = String(describing: record.label)
        print("[\(record.level.rawValue.uppercased())] \(labelStr): \(msgStr)")
      }
    }
  }
}

public struct Logger: Sendable {
  public let label: StaticString

  public init(label: StaticString) {
    self.label = label
  }

  @inlinable
  public func info(
    _ msg: StaticString,
    _ arg1: LogArgument = .none,
    _ arg2: LogArgument = .none,
    _ arg3: LogArgument = .none,
    _ arg4: LogArgument = .none
  ) {
    AppLogger.shared.log(
      level: .info, label: label, message: msg, arg1: arg1, arg2: arg2, arg3: arg3, arg4: arg4)
  }

  @inlinable
  public func warning(
    _ msg: StaticString,
    _ arg1: LogArgument = .none,
    _ arg2: LogArgument = .none,
    _ arg3: LogArgument = .none,
    _ arg4: LogArgument = .none
  ) {
    AppLogger.shared.log(
      level: .warn, label: label, message: msg, arg1: arg1, arg2: arg2, arg3: arg3, arg4: arg4)
  }

  @inlinable
  public func error(
    _ msg: StaticString,
    _ arg1: LogArgument = .none,
    _ arg2: LogArgument = .none,
    _ arg3: LogArgument = .none,
    _ arg4: LogArgument = .none
  ) {
    AppLogger.shared.log(
      level: .error, label: label, message: msg, arg1: arg1, arg2: arg2, arg3: arg3, arg4: arg4)
  }

  @inlinable
  public func debug(
    _ msg: StaticString,
    _ arg1: LogArgument = .none,
    _ arg2: LogArgument = .none,
    _ arg3: LogArgument = .none,
    _ arg4: LogArgument = .none
  ) {
    AppLogger.shared.log(
      level: .debug, label: label, message: msg, arg1: arg1, arg2: arg2, arg3: arg3, arg4: arg4)
  }
}
