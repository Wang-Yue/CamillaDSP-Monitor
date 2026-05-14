import Foundation

/// Errors raised while parsing or validating a `CamillaDSPConfig`.
public enum ConfigError: Error, Sendable, CustomStringConvertible {
  case parseError(String)
  case validationError(String)
  case invalidFilter(String)
  case invalidMixer(String)
  case invalidPipeline(String)

  public var description: String {
    switch self {
    case .parseError(let msg): return "Parse error: \(msg)"
    case .validationError(let msg): return "Validation error: \(msg)"
    case .invalidFilter(let msg): return "Invalid filter: \(msg)"
    case .invalidMixer(let msg): return "Invalid mixer: \(msg)"
    case .invalidPipeline(let msg): return "Invalid pipeline: \(msg)"
    }
  }
}
