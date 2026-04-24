// SpectrumEngine - Lifecycle and data management for spectrum display

import Foundation
import Observation

@MainActor
@Observable
final class SpectrumEngine {
  private(set) var bands: [Double]?

  /// Number of active spectrum views currently on screen.
  var visibilityCount: Int = 0

  init() {}

  /// Update the bands with pre-computed values from the library.
  func updateBands(_ newBands: [Float]) {
    self.bands = newBands.map { Double($0) }
  }

  func reset() {
    bands = nil
  }
}
