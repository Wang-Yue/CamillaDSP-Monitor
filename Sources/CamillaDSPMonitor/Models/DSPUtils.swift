import CamillaDSPLib
import Foundation

// MARK: - Shared Audio Utilities

/// Normalize a dB value (-60..0) to 0..1 range for meter/spectrum display.
func normalizedDB(_ db: Double) -> Double {
  max(0, min(1, (db + 60) / 60))
}

private let _rateFormatter: NumberFormatter = {
  let f = NumberFormatter()
  f.numberStyle = .decimal
  return f
}()

/// Format a sample rate with thousands separator (e.g. "48,000 Hz").
func formatRate(_ rate: Int) -> String {
  (_rateFormatter.string(from: NSNumber(value: rate)) ?? "\(rate)") + " Hz"
}

// MARK: - Biquad Coefficients

public struct BiquadCoefficients: Sendable {
  public var b0, b1, b2, a1, a2: Double

  public static func compute(_ type: String, freq: Double, gain: Double, q: Double, sampleRate: Int)
    -> BiquadCoefficients?
  {
    let fs = Double(sampleRate)
    let w0 = 2.0 * .pi * freq / fs
    let cosW0 = cos(w0)
    let alpha = sin(w0) / (2.0 * q)
    let A = pow(10.0, gain / 40.0)
    var b0: Double
    var b1: Double
    var b2: Double
    var a0: Double
    var a1: Double
    var a2: Double
    switch type {
    case "Peaking":
      b0 = 1 + alpha * A
      b1 = -2 * cosW0
      b2 = 1 - alpha * A
      a0 = 1 + alpha / A
      a1 = -2 * cosW0
      a2 = 1 - alpha / A
    case "Lowshelf":
      b0 = A * ((A + 1) - (A - 1) * cosW0 + 2 * sqrt(A) * alpha)
      b1 = 2 * A * ((A - 1) - (A + 1) * cosW0)
      b2 = A * ((A + 1) - (A - 1) * cosW0 - 2 * sqrt(A) * alpha)
      a0 = (A + 1) + (A - 1) * cosW0 + 2 * sqrt(A) * alpha
      a1 = -2 * ((A - 1) + (A + 1) * cosW0)
      a2 = (A + 1) + (A - 1) * cosW0 - 2 * sqrt(A) * alpha
    case "Highshelf":
      b0 = A * ((A + 1) + (A - 1) * cosW0 + 2 * sqrt(A) * alpha)
      b1 = -2 * A * ((A - 1) + (A + 1) * cosW0)
      b2 = A * ((A + 1) + (A - 1) * cosW0 - 2 * sqrt(A) * alpha)
      a0 = (A + 1) - (A - 1) * cosW0 + 2 * sqrt(A) * alpha
      a1 = 2 * ((A - 1) - (A + 1) * cosW0)
      a2 = (A + 1) - (A - 1) * cosW0 - 2 * sqrt(A) * alpha
    default: return nil
    }
    return BiquadCoefficients(b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0)
  }
}
