import CoreGraphics
import Foundation

/// Shared helpers for DSP UI graphs and plots.
public struct LogScaleAxis {
  public let minFreq: Double
  public let maxFreq: Double
  @usableFromInline internal let logMin: Double
  @usableFromInline internal let logMax: Double
  @usableFromInline internal let dLog: Double

  public init(minFreq: Double = 20, maxFreq: Double = 20_000) {
    self.minFreq = minFreq
    self.maxFreq = maxFreq
    self.logMin = log10(minFreq)
    self.logMax = log10(maxFreq)
    self.dLog = logMax - logMin
  }

  /// Map frequency to normalized [0, 1] x-coordinate.
  @inlinable
  public func xFrac(for freq: Double) -> Double {
    (log10(max(freq, minFreq)) - logMin) / dLog
  }

  /// Map frequency to pixel X coordinate.
  @inlinable
  public func x(for freq: Double, width: Double) -> Double {
    xFrac(for: freq) * width
  }

  /// Map pixel X back to frequency.
  @inlinable
  public func frequency(for x: Double, width: Double) -> Double {
    let frac = x / width
    let logF = logMin + frac * dLog
    return pow(10, logF)
  }
}

/// Shared utility for formatting audio frequencies.
@inlinable
public func formatFrequency(_ f: Double) -> String {
  if f >= 1000 {
    let k = f / 1000
    let s = String(format: "%.2f", k)
    let parts = s.split(separator: ".")
    let intPart = parts[0]
    let fracPart =
      parts.count > 1 ? parts[1].trimmingCharacters(in: CharacterSet(charactersIn: "0")) : ""

    if fracPart.isEmpty {
      return "\(intPart)k"
    } else {
      return "\(intPart)k\(fracPart)"
    }
  } else {
    return "\(Int(f.rounded()))"
  }
}

@inlinable
public func formatFrequency(_ f: Float) -> String {
  formatFrequency(Double(f))
}
