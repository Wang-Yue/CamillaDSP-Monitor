import Foundation

/// Errors raised by the audio I/O backends (capture and playback).
internal enum BackendError: Error, Sendable, CustomStringConvertible {
  case deviceNotFound(String)
  case initializationFailed(String)
  case readError(String)
  case writeError(String)

  internal var description: String {
    switch self {
    case .deviceNotFound(let msg): return "Device not found: \(msg)"
    case .initializationFailed(let msg): return "Initialization failed: \(msg)"
    case .readError(let msg): return "Read error: \(msg)"
    case .writeError(let msg): return "Write error: \(msg)"
    }
  }
}
