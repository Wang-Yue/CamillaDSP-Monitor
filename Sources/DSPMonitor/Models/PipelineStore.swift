// PipelineStore - Pipeline stage and EQ preset management with persistence

import Foundation
import Observation

@MainActor
@Observable
final class PipelineStore {
  let defaults = UserDefaults.standard

  var stages: [PipelineStage] = []
  var eqPresets: [EQPreset] = []
  var convPresets: [ConvolutionPreset] = []

  /// Fired after any change that requires a DSP config rebuild.
  var onChanged: (() -> Void)?

  init() {
    // Loaded by AppState during bootstrap
  }

  // MARK: - Pipeline Stage Management & Persistence

  func savePipelineStages() {
    let snapshots = stages.map { $0.toSnapshot() }
    if let data = try? JSONEncoder().encode(snapshots) {
      defaults.set(data, forKey: "pipelineStages")
    }
  }

  func loadPipelineStages() {
    guard let data = defaults.data(forKey: "pipelineStages"),
      let snapshots = try? JSONDecoder().decode([PipelineStage.Snapshot].self, from: data)
    else {
      // Fallback to default stages if none are saved
      self.stages = PipelineStage.defaultStages()
      return
    }
    self.stages = snapshots.map { snap in
      let type = StageType(rawValue: snap.stageType) ?? .eq
      let stage = PipelineStage(
        id: snap.id,
        type: type,
        name: snap.name,
        isEnabled: snap.isEnabled,
        channels: Set(snap.channels)
      )
      stage.restore(from: snap)
      return stage
    }
  }

  func addStage(type: StageType) {
    let stage = PipelineStage(type: type, isEnabled: true)
    stages.append(stage)
    savePipelineStages()
    onChanged?()
  }

  func deleteStage(id: UUID) {
    stages.removeAll { $0.id == id }
    savePipelineStages()
    onChanged?()
  }

  func moveStages(from source: IndexSet, to destination: Int) {
    stages.move(fromOffsets: source, toOffset: destination)
    savePipelineStages()
    onChanged?()
  }

  // MARK: - EQ Preset Persistence

  func saveEQPresets() {
    if let data = try? JSONEncoder().encode(eqPresets) {
      defaults.set(data, forKey: "eqPresets")
    }
  }

  func loadEQPresets() -> [EQPreset] {
    guard let data = defaults.data(forKey: "eqPresets"),
      let presets = try? JSONDecoder().decode([EQPreset].self, from: data)
    else { return [] }
    return presets
  }

  func addEQPreset(name: String = "New Preset", preamp: Double = -6.0, bands: [EQBand]? = nil) {
    let preset = EQPreset(
      name: name,
      preampGain: preamp,
      bands: bands ?? [
        EQBand(type: .peaking, freq: 100, gain: 0, q: 1.0),
        EQBand(type: .peaking, freq: 1000, gain: 0, q: 1.0),
        EQBand(type: .peaking, freq: 10000, gain: 0, q: 1.0),
      ])
    eqPresets.append(preset)
    saveEQPresets()
  }

  func deleteEQPreset(at index: Int) {
    guard eqPresets.indices.contains(index) else { return }
    let presetToDelete = eqPresets[index]
    for stage in stages {
      if stage.eqPresetID == presetToDelete.id { stage.eqPresetID = nil }
    }
    eqPresets.remove(at: index)
    saveEQPresets()
    onChanged?()
  }

  // MARK: - Convolution Preset Persistence

  func saveConvPresets() {
    if let data = try? JSONEncoder().encode(convPresets) {
      defaults.set(data, forKey: "convPresets")
    }
  }

  func loadConvPresets() -> [ConvolutionPreset] {
    guard let data = defaults.data(forKey: "convPresets"),
      let presets = try? JSONDecoder().decode([ConvolutionPreset].self, from: data)
    else { return [] }
    return presets
  }

  func addConvolutionPreset(_ preset: ConvolutionPreset) {
    convPresets.append(preset)
    saveConvPresets()
  }

  func deleteConvPreset(at index: Int) {
    guard convPresets.indices.contains(index) else { return }
    let toDelete = convPresets[index]
    for stage in stages {
      if stage.convPresetID == toDelete.id { stage.convPresetID = nil }
    }

    // Delete associated files on disk
    let fm = FileManager.default
    for path in toDelete.irPaths.values {
      if fm.fileExists(atPath: path) {
        try? fm.removeItem(atPath: path)
      }
    }

    convPresets.remove(at: index)
    saveConvPresets()
    onChanged?()
  }

  func updateConvPreset() {
    saveConvPresets()
    onChanged?()
  }

  func channelCount(beforeStageAtIndex index: Int, captureChannels: Int) -> Int {
    var current = captureChannels
    for i in 0..<index {
      guard i < stages.count else { break }
      let stage = stages[i]
      if stage.isEnabled && stage.type == .mixer {
        current = stage.mixerChannelsOut
      }
    }
    return current
  }
}
