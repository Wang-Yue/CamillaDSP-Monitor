import Observation
import SwiftUI

struct GeneralSettingsView: View {
  @Environment(AudioSettings.self) var settings
  @Environment(MonitoringController.self) var monitoring

  var body: some View {
    @Bindable var bindableMonitoring = monitoring

    Form {
      Section("Polling Rate") {
        VStack(alignment: .leading, spacing: 10) {
          HStack {
            Text("Polling Rate")
              .frame(width: 120, alignment: .leading)
            Slider(value: $bindableMonitoring.pollingRate, in: 1...60)
            Text("\(Int(monitoring.pollingRate)) Hz")
              .font(.system(.body, design: .monospaced))
              .foregroundStyle(.secondary)
              .frame(minWidth: 70, alignment: .trailing)
          }

          Text("Adjust the frequency of UI updates for meters and spectrum.")
            .font(.caption)
            .foregroundStyle(.secondary)
        }
        .padding(.vertical, 8)
      }

      Section("Silence Detection") {
        VStack(alignment: .leading, spacing: 10) {
          HStack {
            Text("Silence Threshold")
              .frame(width: 120, alignment: .leading)
            Slider(
              value: Binding(
                get: { Double(settings.silenceThreshold) },
                set: { settings.silenceThreshold = Int($0) }
              ), in: -120...0)
            Text("\(settings.silenceThreshold) dB")
              .font(.system(.body, design: .monospaced))
              .foregroundStyle(.secondary)
              .frame(width: 60, alignment: .trailing)
          }

          HStack {
            Text("Silence Timeout")
              .frame(width: 120, alignment: .leading)
            Slider(
              value: Binding(
                get: { Double(settings.silenceTimeout) },
                set: { settings.silenceTimeout = Int($0) }
              ), in: 0...60)
            if settings.silenceTimeout == 0 {
              Text("Disabled")
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(width: 60, alignment: .trailing)
            } else {
              Text("\(settings.silenceTimeout) s")
                .font(.system(.body, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(width: 60, alignment: .trailing)
            }
          }

          Text("Pause processing if the input signal is silent for the specified duration.")
            .font(.caption)
            .foregroundStyle(.secondary)
        }
        .padding(.vertical, 8)
      }
    }
    .formStyle(.grouped)
    .frame(width: 450, height: 320)
  }
}
