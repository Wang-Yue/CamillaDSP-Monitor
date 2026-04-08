// AppState+Pipeline - Pipeline stage persistence

import Foundation

extension AppState {

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

    // Match snapshots to stages by type
    for stage in stages {
      if let snap = snapshots.first(where: { $0.stageType == stage.type.rawValue }) {
        stage.restore(from: snap)
      }
    }
  }
}
