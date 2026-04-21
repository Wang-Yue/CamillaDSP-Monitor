// DevicePickerView - Audio device selection and configuration

import CamillaDSPLib
import SwiftUI

struct DevicePickerView: View {
  @EnvironmentObject var devices: AudioDeviceManager
  @EnvironmentObject var settings: AudioSettings
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
              Text(settings.camillaDSPPath.isEmpty ? "Not selected" : settings.camillaDSPPath)
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(settings.camillaDSPPath.isEmpty ? .red : .secondary)
                .lineLimit(1)
                .truncationMode(.head)

              Spacer()

              Button("Browse...") {
                selectBinary()
              }
            }

            if settings.camillaDSPPath.isEmpty {
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
          devices: devices.captureDevices,
          selectedDevice: Binding(
            get: { devices.captureConfig.deviceName },
            set: { devices.captureConfig.deviceName = $0 }),
          channels: $devices.captureConfig.channels,
          supportedChannels: devices.captureConfig.supportedChannels
        ) {
          VStack(alignment: .leading, spacing: 8) {
            HStack {
              Text("Sample Rate")
                .frame(width: 100, alignment: .leading)
              if settings.resamplerEnabled {
                Picker("", selection: $devices.captureConfig.sampleRate) {
                  ForEach(devices.captureRateOptions, id: \.self) { rate in
                    Text(formatRate(rate)).tag(rate)
                  }
                }
                .labelsHidden()
              } else {
                Text(formatRate(devices.captureConfig.sampleRate))
                  .font(.system(.body, design: .monospaced))
                  .foregroundStyle(.secondary)
              }
            }

            HStack {
              Text("Format")
                .frame(width: 100, alignment: .leading)
              if devices.captureConfig.supportedFormats.isEmpty {
                Text(devices.captureConfig.format)
                  .font(.system(.body, design: .monospaced))
                  .foregroundStyle(.secondary)
              } else {
                Picker("", selection: $devices.captureConfig.format) {
                  ForEach(devices.captureConfig.supportedFormats, id: \.self) { fmt in
                    Text(fmt).tag(fmt)
                  }
                }
                .labelsHidden()
              }
            }

            if !settings.resamplerEnabled {
              Text("Follows the playback sample rate (enable Resampler for independent rates)")
                .font(.caption)
                .foregroundStyle(.tertiary)
            }
          }
        }

        // Playback device
        DeviceSection(
          title: "Playback (Output)",
          icon: "hifispeaker.2.fill",
          iconColor: .green,
          devices: devices.playbackDevices,
          selectedDevice: Binding(
            get: { devices.playbackConfig.deviceName },
            set: { devices.playbackConfig.deviceName = $0 }),
          channels: $devices.playbackConfig.channels,
          supportedChannels: devices.playbackConfig.supportedChannels
        ) {
          VStack(alignment: .leading, spacing: 8) {
            HStack {
              Text("Sample Rate")
                .frame(width: 100, alignment: .leading)
              Picker("", selection: $devices.playbackConfig.sampleRate) {
                ForEach(devices.playbackRateOptions, id: \.self) { rate in
                  Text(formatRate(rate)).tag(rate)
                }
              }
              .labelsHidden()
            }

            HStack {
              Text("Format")
                .frame(width: 100, alignment: .leading)
              if devices.playbackConfig.supportedFormats.isEmpty {
                Text(devices.playbackConfig.format)
                  .font(.system(.body, design: .monospaced))
                  .foregroundStyle(.secondary)
              } else {
                Picker("", selection: $devices.playbackConfig.format) {
                  ForEach(devices.playbackConfig.supportedFormats, id: \.self) { fmt in
                    Text(fmt).tag(fmt)
                  }
                }
                .labelsHidden()
              }
            }

            Toggle("Exclusive Mode (Hog)", isOn: $devices.exclusiveMode)
            Text(
              "Takes exclusive access to the output device, preventing other apps from using it"
            )
            .font(.caption)
            .foregroundStyle(.secondary)
          }
        }

        // Processing settings
        GroupBox {
          VStack(alignment: .leading, spacing: 12) {
            Label("Processing", systemImage: "cpu")
              .font(.headline)

            HStack {
              Text("Chunk Size")
                .frame(width: 100, alignment: .leading)
              Picker("", selection: $settings.chunkSize) {
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

            Toggle("Enable Rate Adjust", isOn: $settings.enableRateAdjust)
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
    String(format: "(%.1f ms latency)", devices.latencyMs)
  }

  private func selectBinary() {
    let panel = NSOpenPanel()
    panel.allowsMultipleSelection = false
    panel.canChooseDirectories = false
    panel.canChooseFiles = true
    panel.message = "Select camilladsp executable"

    if panel.runModal() == .OK, let url = panel.url {
      settings.camillaDSPPath = url.path
      showRestartAlert = true
    }
  }
}

// MARK: - Device Section

struct DeviceSection<ExtraContent: View>: View {
  let title: String
  let icon: String
  let iconColor: Color
  let devices: [AudioDevice]
  @Binding var selectedDevice: String?
  @Binding var channels: Int
  let supportedChannels: [Int]
  let extraContent: ExtraContent

  init(
    title: String, icon: String, iconColor: Color, devices: [AudioDevice],
    selectedDevice: Binding<String?>, channels: Binding<Int>,
    supportedChannels: [Int] = [],
    @ViewBuilder extraContent: () -> ExtraContent
  ) {
    self.title = title
    self.icon = icon
    self.iconColor = iconColor
    self.devices = devices
    self._selectedDevice = selectedDevice
    self._channels = channels
    self.supportedChannels = supportedChannels
    self.extraContent = extraContent()
  }

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
            .frame(width: 100, alignment: .leading)
          if supportedChannels.isEmpty {
            Stepper("\(channels)", value: $channels, in: 1...32)
              .frame(width: 120)
          } else {
            Picker("", selection: $channels) {
              ForEach(supportedChannels, id: \.self) { ch in
                Text("\(ch)").tag(ch)
              }
            }
            .labelsHidden()
          }
        }

        extraContent
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
