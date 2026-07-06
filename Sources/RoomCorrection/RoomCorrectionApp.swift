import SwiftUI

@main
struct RoomCorrectionApp: App {
  @State private var session: MeasurementSession

  init() {
    let session = MeasurementSession()
    self._session = State(initialValue: session)
  }

  var body: some Scene {
    Window("Room Correction", id: "main") {
      MeasurementView()
        .environment(session)
        .frame(minWidth: 960, minHeight: 680)
    }
    .windowStyle(.titleBar)
    .defaultSize(width: 1100, height: 780)
  }
}
