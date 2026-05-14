public enum LogLevel: String, CaseIterable, Identifiable, Sendable {
  case off = "Off"
  case error = "Error"
  case warn = "Warn"
  case info = "Info"
  case debug = "Debug"
  case trace = "Trace"
  public var id: String { rawValue }

  /// Compact byte encoding for `Atomic<UInt8>` storage in
  /// `MutableLogLevel`. The exact mapping is internal.
  public var rawByte: UInt8 {
    switch self {
    case .off: return 0
    case .error: return 1
    case .warn: return 2
    case .info: return 3
    case .debug: return 4
    case .trace: return 5
    }
  }

  public init(rawByte: UInt8) {
    switch rawByte {
    case 0: self = .off
    case 1: self = .error
    case 2: self = .warn
    case 4: self = .debug
    case 5: self = .trace
    default: self = .info
    }
  }
}
