// EQPresetDetailView - Biquad EQ preset editor with three modes

import CamillaDSPLib
import SwiftUI

enum EQEditMode: String, CaseIterable {
  case diagram = "Diagram"
  case form = "Form"
  case csv = "CSV"
  var icon: String {
    switch self {
    case .diagram: return "waveform.path.ecg"
    case .form: return "slider.horizontal.3"
    case .csv: return "doc.plaintext"
    }
  }
}

struct EQPresetDetailView: View {
  @ObservedObject var preset: EQPreset
  @EnvironmentObject var appState: AppState
  @State private var editMode: EQEditMode = .diagram
  @State private var selectedBandID: UUID?

  var body: some View {
    VStack(spacing: 0) {
      HStack {
        Image(systemName: "slider.horizontal.3").font(.title2).foregroundStyle(Color.accentColor)
        TextField("Preset Name", text: $preset.name).font(.title2.bold()).textFieldStyle(
          .roundedBorder
        ).frame(maxWidth: 300).onSubmit { NSApp.keyWindow?.makeFirstResponder(nil) }.onChange(
          of: preset.name
        ) { _, _ in appState.saveEQPresets() }
        Spacer()
        Picker("", selection: $editMode) {
          ForEach(EQEditMode.allCases, id: \.rawValue) { mode in
            Label(mode.rawValue, systemImage: mode.icon).tag(mode)
          }
        }.pickerStyle(.segmented).fixedSize()
      }.padding()

      Divider()

      switch editMode {
      case .diagram:
        EQDiagramMode(
          preset: preset, selectedBandID: $selectedBandID, sampleRate: appState.sampleRate)
      case .form: EQFormMode(preset: preset, selectedBandID: $selectedBandID)
      case .csv: EQCSVMode(preset: preset)
      }
    }
    .background(Color(nsColor: .controlBackgroundColor))
    .onChange(of: preset.bands.count) { _, _ in appState.saveEQPresets() }
    .onReceive(
      preset.objectWillChange
        .debounce(for: .seconds(0.2), scheduler: RunLoop.main)
    ) { _ in
      appState.applyConfig()
    }
  }
}
