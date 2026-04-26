// SpectrumEngine - Lifecycle and data management for spectrum display

import Foundation
import Observation

@MainActor
@Observable
final class SpectrumEngine {
  private(set) var bands: [Double]?
  private(set) var frequencies: [Double]?

  /// Number of active spectrum views currently on screen.
  var visibilityCount: Int = 0

  private let defaults = UserDefaults.standard

  // Spectrum configuration
  var minFreq: Double = 25.0 {
    didSet { defaults.set(minFreq, forKey: "spectrum_min_freq") }
  }
  var maxFreq: Double = 20000.0 {
    didSet { defaults.set(maxFreq, forKey: "spectrum_max_freq") }
  }
  var nBins: UInt32 = 30 {
    didSet { defaults.set(Int(nBins), forKey: "spectrum_n_bins") }
  }
  var side: String = "capture" {
    didSet { defaults.set(side, forKey: "spectrum_side") }
  }

  init() {
    let savedMin = defaults.double(forKey: "spectrum_min_freq")
    if savedMin > 0 { self.minFreq = savedMin }

    let savedMax = defaults.double(forKey: "spectrum_max_freq")
    if savedMax > 0 { self.maxFreq = savedMax }

    let bins = defaults.integer(forKey: "spectrum_n_bins")
    if bins > 0 { self.nBins = UInt32(bins) }

    if let s = defaults.string(forKey: "spectrum_side") {
      self.side = s
    }
  }

  /// Update the spectrum with pre-computed values from the library.
  func updateSpectrum(frequencies: [Float], magnitudes: [Float]) {
    guard !magnitudes.isEmpty else {
      reset()
      return
    }
    self.frequencies = frequencies.map { Double($0) }
    self.bands = magnitudes.map { Double($0) }
  }

  func reset() {
    if bands != nil || frequencies != nil {
      bands = nil
      frequencies = nil
    }
  }

  func resetToDefaults() {
    minFreq = 25.0
    maxFreq = 20000.0
    nBins = 30
    side = "capture"
  }
}
