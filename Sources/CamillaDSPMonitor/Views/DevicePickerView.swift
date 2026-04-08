// DevicePickerView - Audio device selection and configuration

import CamillaDSPLib
import SwiftUI

func formatRate(_ rate: Int) -> String {
  let formatter = NumberFormatter()
  formatter.numberStyle = .decimal
  return (formatter.string(from: NSNumber(value: rate)) ?? "\(rate)") + " Hz"
}

struct DevicePickerView: View {
  @EnvironmentObject var appState: AppState
  @State private var showRestartAlert = false

  var body: some View {
    ScrollView {
      VStack(spacing: 20) {
        // Engine path
        GroupBox {
          VStack(alignment: .leading, spacing: 12) {
            Label("CamillaDSP Engine", systemImage: "gearshape.2.fill")
              .font(.headline)

            HStack {
              Text(appState.camillaDSPPath.isEmpty ? "Not selected" : appState.camillaDSPPath)
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(appState.camillaDSPPath.isEmpty ? .red : .secondary)
                .lineLimit(1)
                .truncationMode(.head)

              Spacer()

              Button("Browse...") {
                selectBinary()
              }
            }

            if appState.camillaDSPPath.isEmpty {
              Text("Please select the camilladsp executable to start the engine.")
                .font(.caption)
                .foregroundStyle(.secondary)
            }
          }
          .padding(4)
          .frame(maxWidth: .infinity, alignment: .leading)
        }

        // Capture device
        DeviceSection(
          title: "Capture (Input)",
          icon: "mic.fill",
          iconColor: .blue,
          devices: appState.captureDevices,
          selectedDevice: $appState.selectedCaptureDevice,
          channels: $appState.captureChannels,
          extraContent: AnyView(
            VStack(alignment: .leading, spacing: 8) {
              HStack {
                Text("Sample Rate")
                  .frame(width: 100, alignment: .leading)
                if appState.resamplerEnabled {
                  Picker("", selection: $appState.captureSampleRate) {
                    ForEach(appState.captureSupportedRates, id: \.self) { rate in
                      Text(formatRate(rate)).tag(rate)
                    }
                  }
                  .labelsHidden()
                } else {
                  Text(formatRate(appState.captureSampleRate))
                    .font(.system(.body, design: .monospaced))
                    .foregroundStyle(.secondary)
                }
              }

              HStack {
                Text("Format")
                  .frame(width: 100, alignment: .leading)
                Text(appState.captureFormat)
                  .font(.system(.body, design: .monospaced))
                  .foregroundStyle(.secondary)
              }

              if !appState.resamplerEnabled {
                Text("Follows the playback sample rate (enable Resampler for independent rates)")
                  .font(.caption)
                  .foregroundStyle(.tertiary)
              }
            }
          )
        )

        // Playback device
        DeviceSection(
          title: "Playback (Output)",
          icon: "hifispeaker.2.fill",
          iconColor: .green,
          devices: appState.playbackDevices,
          selectedDevice: $appState.selectedPlaybackDevice,
          channels: $appState.playbackChannels,
          extraContent: AnyView(
            VStack(alignment: .leading, spacing: 8) {
              HStack {
                Text("Sample Rate")
                  .frame(width: 100, alignment: .leading)
                Picker(
                  "",
                  selection: $appState.playbackSampleRate
                ) {
                  ForEach(appState.combinedSupportedRates, id: \.self) { rate in
                    Text(formatRate(rate)).tag(rate)
                  }
                }
                .labelsHidden()
              }

              HStack {
                Text("Format")
                  .frame(width: 100, alignment: .leading)
                Text(appState.playbackFormat)
                  .font(.system(.body, design: .monospaced))
                  .foregroundStyle(.secondary)
              }

              Toggle("Exclusive Mode (Hog)", isOn: $appState.exclusiveMode)
              Text(
                "Takes exclusive access to the output device, preventing other apps from using it"
              )
              .font(.caption)
              .foregroundStyle(.secondary)
            }
          )
        )

        // Processing settings
        GroupBox {
          VStack(alignment: .leading, spacing: 12) {
            Label("Processing", systemImage: "cpu")
              .font(.headline)

            HStack {
              Text("Chunk Size")
                .frame(width: 100, alignment: .leading)
              Picker("", selection: $appState.chunkSize) {
                Text("256 samples").tag(256)
                Text("512 samples").tag(512)
                Text("1024 samples").tag(1024)
                Text("2048 samples").tag(2048)
                Text("4096 samples").tag(4096)
              }
              .labelsHidden()

              Text(latencyText)
                .font(.caption)
                .foregroundStyle(.secondary)
            }

            Toggle("Enable Rate Adjust", isOn: $appState.enableRateAdjust)
            Text("Compensate for clock drift between capture and playback devices")
              .font(.caption)
              .foregroundStyle(.secondary)
          }
          .padding(4)
          .frame(maxWidth: .infinity, alignment: .leading)
        }
      }
      .padding()
    }
    .background(Color(nsColor: .controlBackgroundColor))
    .alert("Engine Path Updated", isPresented: $showRestartAlert) {
      Button("OK", role: .cancel) {}
    } message: {
      Text("Please restart CamillaDSP Monitor to apply the new engine path.")
    }
  }

  private var latencyText: String {
    String(format: "(%.1f ms latency)", appState.latencyMs)
  }

  private func selectBinary() {
    let panel = NSOpenPanel()
    panel.allowsMultipleSelection = false
    panel.canChooseDirectories = false
    panel.canChooseFiles = true
    panel.message = "Select camilladsp executable"

    if panel.runModal() == .OK, let url = panel.url {
      appState.camillaDSPPath = url.path
      showRestartAlert = true
    }
  }

}

// MARK: - Device Section

struct DeviceSection: View {
  let title: String
  let icon: String
  let iconColor: Color
  let devices: [AudioDevice]
  @Binding var selectedDevice: String?
  @Binding var channels: Int
  var extraContent: AnyView? = nil

  var body: some View {
    GroupBox {
      VStack(alignment: .leading, spacing: 12) {
        Label(title, systemImage: icon)
          .font(.headline)
          .foregroundStyle(iconColor)

        if devices.isEmpty {
          HStack {
            Image(systemName: "exclamationmark.triangle")
              .foregroundStyle(.orange)
            Text("No devices found")
              .foregroundStyle(.secondary)
          }
        } else {
          VStack(alignment: .leading, spacing: 6) {
            DeviceRow(
              name: "System Default",
              isSelected: selectedDevice == nil,
              onSelect: { selectedDevice = nil }
            )

            Divider()

            ForEach(devices) { device in
              DeviceRow(
                name: device.name,
                isSelected: selectedDevice == device.name,
                onSelect: { selectedDevice = device.name }
              )
            }
          }
        }

        HStack {
          Text("Channels")
            .foregroundStyle(.secondary)
          Stepper("\(channels)", value: $channels, in: 1...32)
            .frame(width: 120)
        }

        if let extra = extraContent {
          extra
        }
      }
      .padding(4)
      .frame(maxWidth: .infinity, alignment: .leading)
    }
  }
}

struct DeviceRow: View {
  let name: String
  var detail: String? = nil
  let isSelected: Bool
  let onSelect: () -> Void

  var body: some View {
    Button(action: onSelect) {
      HStack {
        Image(systemName: isSelected ? "checkmark.circle.fill" : "circle")
          .foregroundStyle(isSelected ? Color.accentColor : Color.secondary)

        VStack(alignment: .leading) {
          Text(name)
            .foregroundStyle(.primary)
          if let detail = detail {
            Text(detail)
              .font(.caption)
              .foregroundStyle(.tertiary)
          }
        }

        Spacer()

        if isSelected {
          Image(systemName: "checkmark")
            .foregroundStyle(Color.accentColor)
            .font(.caption)
        }
      }
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .padding(.vertical, 2)
  }
}
