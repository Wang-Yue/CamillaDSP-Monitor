import Observation
import SwiftUI

struct GeneralSettingsView: View {
  @Environment(AudioSettings.self) var settings
  @Environment(MonitoringController.self) var monitoring

  var body: some View {
    @Bindable var bindableMonitoring = monitoring

    Form {
      Section {
        VStack(alignment: .leading, spacing: 10) {
          HStack {
            Text("Polling Rate")
            Slider(value: $bindableMonitoring.pollingRate, in: 1...60, step: 1)
            Text("\(Int(monitoring.pollingRate)) Hz")
              .font(.system(.body, design: .monospaced))
              .foregroundStyle(.secondary)
          }

          Text("Adjust the frequency of UI updates for meters and spectrum.")
            .font(.caption)
            .foregroundStyle(.secondary)
        }
        .padding(.vertical, 8)
      }
    }
    .formStyle(.grouped)
    .frame(width: 450, height: 140)
  }
}
