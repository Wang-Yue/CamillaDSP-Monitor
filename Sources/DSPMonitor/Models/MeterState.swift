// MeterState - Split observable state for UI binding
//
// Split into two independent ObservableObjects so that level changes
// don't cause load views to redraw. This reduces SwiftUI's AttributeGraph updates.

import Foundation
import Observation

/// Peak/RMS levels for capture and playback — observed by meter views.
@MainActor
@Observable
final class LevelState {
  var visibilityCount: Int = 0
  var capturePeak: [Float] = []
  var captureRms: [Float] = []
  var playbackPeak: [Float] = []
  var playbackRms: [Float] = []

  func update(
    capturePeak: [Float], captureRms: [Float],
    playbackPeak: [Float], playbackRms: [Float]
  ) {
    self.capturePeak = capturePeak
    self.captureRms = captureRms
    self.playbackPeak = playbackPeak
    self.playbackRms = playbackRms
  }

  func reset(captureChannels: Int, playbackChannels: Int) {
    let capSilent = Array(repeating: Float(-100.0), count: captureChannels)
    let playSilent = Array(repeating: Float(-100.0), count: playbackChannels)

    if capturePeak != capSilent || captureRms != capSilent || playbackPeak != playSilent
      || playbackRms != playSilent
    {
      update(
        capturePeak: capSilent,
        captureRms: capSilent,
        playbackPeak: playSilent,
        playbackRms: playSilent
      )
    }
  }

  func clear() {
    if !capturePeak.isEmpty || !captureRms.isEmpty || !playbackPeak.isEmpty || !playbackRms.isEmpty
    {
      update(capturePeak: [], captureRms: [], playbackPeak: [], playbackRms: [])
    }
  }
}
