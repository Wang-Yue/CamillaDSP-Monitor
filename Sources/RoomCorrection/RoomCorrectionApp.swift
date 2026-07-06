import SwiftUI

@main
struct RoomCorrectionApp: App {
  @State private var session: MeasurementSession
  @State private var dsp: RoomCorrectionEngineController

  init() {
    let session = MeasurementSession()
    self._session = State(initialValue: session)
    self._dsp = State(initialValue: RoomCorrectionEngineController(session: session))
  }

  var body: some Scene {
    Window("Room Correction", id: "main") {
      MeasurementView()
        .environment(session)
        .environment(dsp)
        .frame(minWidth: 960, minHeight: 680)
    }
    .windowStyle(.titleBar)
    .defaultSize(width: 1100, height: 780)
  }
}
