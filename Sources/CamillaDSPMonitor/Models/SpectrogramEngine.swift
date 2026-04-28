// SpectrogramEngine - Lifecycle and data management for spectroscope display

import Foundation
import Observation

struct SpectrogramFrame: Sendable {
  let data: [Float]
  let timestamp: Date
}

@MainActor
@Observable
final class SpectrogramEngine {
  private(set) var bands: [Float]?
  private(set) var frequencies: [Float]?

  // Spectrogram history
  private(set) var history: [SpectrogramFrame] = []
  let timeWindow: TimeInterval = 10.0

  /// Number of active views currently on screen.
  var visibilityCount: Int = 0

  private let defaults = UserDefaults.standard

  // Spectrogram configuration
  // Fixed range as requested by user
  let minFreq: Double = 20.0
  let maxFreq: Double = 20000.0

  var nBins: UInt32 = 200 {
    didSet {
      defaults.set(Int(nBins), forKey: "spectroscope_n_bins")
      history.removeAll()
    }
  }
  var isCapture: Bool = true {
    didSet { defaults.set(isCapture, forKey: "spectroscope_is_capture") }
  }

  enum Palette: String, CaseIterable, Codable {
    case heatMap = "Heat Map"
    case grayscale = "Grayscale"
    case appTheme = "App Theme"
  }

  var selectedPalette: Palette = .heatMap {
    didSet { defaults.set(selectedPalette.rawValue, forKey: "spectroscope_palette") }
  }

  init() {
    let bins = defaults.integer(forKey: "spectroscope_n_bins")
    if bins > 0 { self.nBins = UInt32(bins) }

    if defaults.object(forKey: "spectroscope_is_capture") != nil {
      self.isCapture = defaults.bool(forKey: "spectroscope_is_capture")
    }

    if let savedPalette = defaults.string(forKey: "spectroscope_palette"),
      let palette = Palette(rawValue: savedPalette)
    {
      self.selectedPalette = palette
    }
  }

  /// Update the spectrum with pre-computed values from the library.
  func updateSpectrum(frequencies: [Float], magnitudes: [Float]) {
    guard !magnitudes.isEmpty else {
      reset()
      return
    }
    self.frequencies = frequencies
    self.bands = magnitudes

    // Update history for spectrogram
    let frame = SpectrogramFrame(data: magnitudes, timestamp: Date())
    history.append(frame)

    // Filter out old frames
    let cutoff = Date().addingTimeInterval(-timeWindow)
    history.removeAll { $0.timestamp < cutoff }
  }

  func reset() {
    bands = nil
    history.removeAll()
  }

  func resetToDefaults() {
    nBins = 200
    isCapture = true
    selectedPalette = .heatMap
    history.removeAll()
  }
}
