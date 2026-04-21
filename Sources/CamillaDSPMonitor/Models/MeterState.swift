// MeterState - Split observable state for UI binding
//
// Split into two independent ObservableObjects so that level changes
// don't cause load views to redraw, and processing load changes don't
// cause meter views to redraw. This reduces SwiftUI's AttributeGraph updates.

import Foundation

struct StereoLevel: Sendable, Equatable {
  var left: Double
  var right: Double
  static let silent = StereoLevel(left: -100, right: -100)

  init(left: Double, right: Double) {
    self.left = left
    self.right = right
  }

  init(from levels: [Float]) {
    left = max(-100.0, Double(levels.first ?? -100))
    right = max(-100.0, Double(levels.count > 1 ? levels[1] : -100))
  }
}

/// Peak/RMS levels for capture and playback — observed by meter views.
@MainActor
final class LevelState: ObservableObject {
  var capturePeak: StereoLevel = .silent
  var captureRms: StereoLevel = .silent
  var playbackPeak: StereoLevel = .silent
  var playbackRms: StereoLevel = .silent

  func update(
    capturePeak: StereoLevel, captureRms: StereoLevel,
    playbackPeak: StereoLevel, playbackRms: StereoLevel
  ) {
    let unchanged =
      self.capturePeak == capturePeak && self.captureRms == captureRms
      && self.playbackPeak == playbackPeak && self.playbackRms == playbackRms
    guard !unchanged else { return }
    self.capturePeak = capturePeak
    self.captureRms = captureRms
    self.playbackPeak = playbackPeak
    self.playbackRms = playbackRms
    objectWillChange.send()
  }

  func reset() {
    update(
      capturePeak: .silent, captureRms: .silent,
      playbackPeak: .silent, playbackRms: .silent)
  }
}

/// Processing load — observed by CPU usage display.
@MainActor
final class LoadState: ObservableObject {
  var processingLoad: Double = 0

  func update(load: Double) {
    guard self.processingLoad != load else { return }
    self.processingLoad = load
    objectWillChange.send()
  }

  func reset() {
    update(load: 0)
  }
}
