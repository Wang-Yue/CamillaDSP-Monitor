// EQCSVMode - AutoEq / EqualizerAPO compatible text editor for EQ presets

import Observation
import SwiftUI

struct EQCSVMode: View {
  @Bindable var preset: EQPreset
  @Environment(DSPEngineController.self) var dsp
  @State private var csvText: String = ""
  @State private var parseError: String?
  @State private var copyFeedback: Bool = false

  var body: some View {
    VStack(spacing: 0) {
      HStack {
        VStack(alignment: .leading, spacing: 2) {
          Text("AutoEq / EqualizerAPO format")
            .font(.subheadline.bold())
          Text("Edit and Apply, or paste from AutoEq output")
            .font(.caption)
            .foregroundStyle(.secondary)
        }
        Spacer()
        Button {
          csvText = preset.toCSV()
          NSPasteboard.general.clearContents()
          NSPasteboard.general.setString(csvText, forType: .string)
          copyFeedback = true
          DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { copyFeedback = false }
        } label: {
          Label(
            copyFeedback ? "Copied!" : "Copy Text",
            systemImage: copyFeedback ? "checkmark" : "doc.on.doc")
        }
        Button("Refresh") {
          csvText = preset.toCSV()
        }
        Button("Apply") {
          if let result = EQPreset.fromCSV(csvText) {
            preset.preampGain = result.preamp
            preset.bands = result.bands
            dsp.applyConfig()
            parseError = nil
          } else {
            parseError = "Failed to parse — check format (expecting 'Filter 1: ON PK Fc...')"
          }
        }
        .buttonStyle(.borderedProminent)
      }
      .padding()

      if let error = parseError {
        Text(error)
          .font(.caption)
          .foregroundStyle(.red)
          .padding(.horizontal)
          .padding(.bottom, 8)
      }

      Divider()

      TextEditor(text: $csvText)
        .font(.system(.body, design: .monospaced))
        .padding(4)
    }
    .onAppear {
      csvText = preset.toCSV()
    }
  }
}
