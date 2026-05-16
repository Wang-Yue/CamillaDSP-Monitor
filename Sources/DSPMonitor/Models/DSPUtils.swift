import Foundation

// MARK: - Shared Audio Constants

// MARK: - Shared Audio Utilities

/// Normalize a dB value (-60..0) to 0..1 range for meter/spectrum display.
func normalizedDB(_ db: Float) -> Double {
  max(0, min(1, (Double(db) + 60) / 60))
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
