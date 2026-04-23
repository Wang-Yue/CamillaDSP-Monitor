// EQPresetDetailView - Biquad EQ preset editor with three modes

import CamillaDSPLib
import Observation
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
  @Bindable var preset: EQPreset
  @Environment(DSPEngineController.self) var dsp
  @Environment(PipelineStore.self) var pipeline
  @Environment(AudioDeviceManager.self) var devices
  @State private var editMode: EQEditMode = .diagram
  @State private var selectedBandID: UUID?

  var body: some View {
    VStack(spacing: 0) {
      HStack {
        Image(systemName: "slider.horizontal.3").font(.title2).foregroundStyle(Color.accentColor)
        TextField("Preset Name", text: $preset.name).font(.title2.bold()).textFieldStyle(
          .roundedBorder
        ).frame(maxWidth: 300).onSubmit { NSApp.keyWindow?.makeFirstResponder(nil) }
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
          preset: preset, selectedBandID: $selectedBandID,
          sampleRate: devices.captureConfig.sampleRate)
      case .form: EQFormMode(preset: preset, selectedBandID: $selectedBandID)
      case .csv: EQCSVMode(preset: preset)
      }
    }
    .background(Color(nsColor: .controlBackgroundColor))
    .onChange(of: preset.bands.count) { _, _ in
      dsp.applyConfig()
    }
    .onChange(of: preset.preampGain) { _, _ in
      dsp.applyConfig()
    }
    .onChange(of: preset.name) { _, _ in
      pipeline.saveEQPresets()
    }
  }
}
