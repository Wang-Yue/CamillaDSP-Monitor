// VectorScopeEngine - Lifecycle and data management for vector scope display

import Foundation
import Observation

enum VectorScopeWindow: String, CaseIterable, Identifiable {
  case fast = "fast"
  case smooth = "smooth"
  case long = "long"

  var id: String { rawValue }

  var title: String {
    switch self {
    case .fast: return "Fast / Snappy (Δt = 25 ms)"
    case .smooth: return "Smooth Persistence (Δt = 50 ms)"
    case .long: return "Long Glow / Trace (Δt = 100 ms)"
    }
  }

  var seconds: Double {
    switch self {
    case .fast: return 0.025
    case .smooth: return 0.050
    case .long: return 0.100
    }
  }
}

@MainActor
@Observable
final class VectorScopeEngine {
  private(set) var leftSamples: [Float] = []
  private(set) var rightSamples: [Float] = []

  /// Number of active vector scope views currently on screen.
  var visibilityCount: Int = 0 {
    didSet {
      if visibilityCount < 0 { visibilityCount = 0 }
    }
  }

  private let defaults = UserDefaults.standard

  // Configuration
  var window: VectorScopeWindow = .fast {
    didSet { defaults.set(window.rawValue, forKey: "vectorscope_window") }
  }
  var isCapture: Bool = true {
    didSet { defaults.set(isCapture, forKey: "vectorscope_is_capture") }
  }
  var showParticles: Bool = true {
    didSet { defaults.set(showParticles, forKey: "vectorscope_show_particles") }
  }
  var autoScale: Bool = true {
    didSet { defaults.set(autoScale, forKey: "vectorscope_auto_scale") }
  }

  init() {
    if let savedWindow = defaults.string(forKey: "vectorscope_window"),
      let opt = VectorScopeWindow(rawValue: savedWindow)
    {
      self.window = opt
    }

    if defaults.object(forKey: "vectorscope_is_capture") != nil {
      self.isCapture = defaults.bool(forKey: "vectorscope_is_capture")
    }

    if defaults.object(forKey: "vectorscope_show_particles") != nil {
      self.showParticles = defaults.bool(forKey: "vectorscope_show_particles")
    }

    if defaults.object(forKey: "vectorscope_auto_scale") != nil {
      self.autoScale = defaults.bool(forKey: "vectorscope_auto_scale")
    }
  }

  /// Calculates the exact number of frames to fetch based on active sample rate and selected time window.
  func framesToFetch(sampleRate: Double) -> UInt32 {
    let rate = sampleRate > 0 ? sampleRate : 48_000
    let frames = Int(round(rate * window.seconds))
    return UInt32(max(128, min(262_144, frames)))
  }

  /// Update the samples.
  func updateSamples(left: [Float], right: [Float]) {
    self.leftSamples = left
    self.rightSamples = right
  }

  func reset() {
    if !leftSamples.isEmpty || !rightSamples.isEmpty {
      leftSamples = []
      rightSamples = []
    }
  }

  func resetToDefaults() {
    window = .fast
    isCapture = true
    showParticles = true
    autoScale = true
  }
}

