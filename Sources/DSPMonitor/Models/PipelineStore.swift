// PipelineStore - Pipeline stage and EQ preset management with persistence

import Foundation
import Observation

@MainActor
@Observable
final class PipelineStore {
  let defaults = UserDefaults.standard

  var stages: [PipelineStage] = PipelineStage.defaultStages()
  var eqPresets: [EQPreset] = []

  /// Fired after any change that requires a DSP config rebuild (currently only preset deletion).
  var onChanged: (() -> Void)?

  // MARK: - Pipeline Stage Persistence

  func savePipelineStages() {
    let snapshots = stages.map { $0.toSnapshot() }
    if let data = try? JSONEncoder().encode(snapshots) {
      defaults.set(data, forKey: "pipelineStages")
    }
  }

  func loadPipelineStages() {
    guard let data = defaults.data(forKey: "pipelineStages"),
      let snapshots = try? JSONDecoder().decode([PipelineStage.Snapshot].self, from: data)
    else { return }
    for stage in stages {
      if let snap = snapshots.first(where: { $0.stageType == stage.type.rawValue }) {
        stage.restore(from: snap)
      }
    }
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
      if stage.eqLeftPresetID == presetToDelete.id { stage.eqLeftPresetID = nil }
      if stage.eqRightPresetID == presetToDelete.id { stage.eqRightPresetID = nil }
    }
    eqPresets.remove(at: index)
    saveEQPresets()
    onChanged?()
  }

}
