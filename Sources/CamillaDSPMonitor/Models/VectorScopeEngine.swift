// VectorScopeEngine - Lifecycle and data management for vector scope display

import Foundation
import Observation

@MainActor
@Observable
final class VectorScopeEngine {
  private(set) var leftSamples: [Float] = []
  private(set) var rightSamples: [Float] = []

  /// Number of active vector scope views currently on screen.
  var visibilityCount: Int = 0

  private let defaults = UserDefaults.standard

  // Configuration
  var nFrames: UInt32 = 512 {
    didSet { defaults.set(Int(nFrames), forKey: "vectorscope_n_frames") }
  }
  var isCapture: Bool = true {
    didSet { defaults.set(isCapture, forKey: "vectorscope_is_capture") }
  }

  init() {
    let frames = defaults.integer(forKey: "vectorscope_n_frames")
    if frames > 0 { self.nFrames = UInt32(frames) }

    if defaults.object(forKey: "vectorscope_is_capture") != nil {
      self.isCapture = defaults.bool(forKey: "vectorscope_is_capture")
    }
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
    nFrames = 512
    isCapture = true
  }
}
