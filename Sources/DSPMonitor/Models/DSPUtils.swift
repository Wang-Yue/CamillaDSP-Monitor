import Foundation

// MARK: - Shared Audio Constants

// MARK: - Shared Audio Utilities

private let _rateFormatter: NumberFormatter = {
  let f = NumberFormatter()
  f.numberStyle = .decimal
  return f
}()

/// Format a sample rate with thousands separator (e.g. "48,000 Hz").
func formatRate(_ rate: Int) -> String {
  (_rateFormatter.string(from: NSNumber(value: rate)) ?? "\(rate)") + " Hz"
}
