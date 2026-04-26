// MeterState - Split observable state for UI binding
//
// Split into two independent ObservableObjects so that level changes
// don't cause load views to redraw. This reduces SwiftUI's AttributeGraph updates.

import Foundation
import Observation

struct StereoLevel: Sendable, Equatable {
  var left: Float
  var right: Float
  static let silent = StereoLevel(left: -100, right: -100)

  init(left: Float, right: Float) {
    self.left = left
    self.right = right
  }

  init(from levels: [Float]) {
    left = max(-100.0, levels.first ?? -100)
    right = max(-100.0, levels.count > 1 ? levels[1] : -100)
  }
}

/// Peak/RMS levels for capture and playback — observed by meter views.
@MainActor
@Observable
final class LevelState {
  var visibilityCount: Int = 0
  var capturePeak: StereoLevel = .silent
  var captureRms: StereoLevel = .silent
  var playbackPeak: StereoLevel = .silent
  var playbackRms: StereoLevel = .silent

  func update(
    capturePeak: StereoLevel, captureRms: StereoLevel,
    playbackPeak: StereoLevel, playbackRms: StereoLevel
  ) {
    self.capturePeak = capturePeak
    self.captureRms = captureRms
    self.playbackPeak = playbackPeak
    self.playbackRms = playbackRms
  }

  func reset() {
    if capturePeak != .silent || captureRms != .silent || playbackPeak != .silent
      || playbackRms != .silent
    {
      update(
        capturePeak: .silent, captureRms: .silent,
        playbackPeak: .silent, playbackRms: .silent)
    }
  }
}
