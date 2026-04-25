// SpectrumEngine - Lifecycle and data management for spectrum display

import Foundation
import Observation

@MainActor
@Observable
final class SpectrumEngine {
  private(set) var bands: [Double]?

  /// Number of active spectrum views currently on screen.
  var visibilityCount: Int = 0

  // Spectrum configuration
  var minFreq: Double = 25.0
  var maxFreq: Double = 20000.0
  var nBins: UInt32 = 30
  var side: String = "capture"
  var channel: UInt32? = nil

  init() {}

  /// Update the bands with pre-computed values from the library.
  func updateBands(_ newBands: [Float]) {
    self.bands = newBands.map { Double($0) }
  }

  func reset() {
    bands = nil
  }
}
