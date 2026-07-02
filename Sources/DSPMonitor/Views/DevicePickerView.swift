// DevicePickerView - Audio device selection and configuration

import DSPConfig
import DSPLib
import Observation
import SwiftUI

struct DevicePickerView: View {
  @Environment(AudioDeviceManager.self) var devices
  @Environment(AudioSettings.self) var settings

  var body: some View {
    @Bindable var bindableDevices = devices
    @Bindable var bindableSettings = settings
    ScrollView {
      VStack(spacing: 20) {
        // Capture device
        DeviceSection(
          title: "Capture (Input)",
          icon: "mic.fill",
          iconColor: .blue,
          devices: bindableDevices.captureDevices,
          selectedDevice: Binding(
            get: { bindableDevices.captureConfig.deviceName },
            set: { bindableDevices.captureConfig.deviceName = $0 }),
          channels: $bindableDevices.captureConfig.channels,
          deviceChannels: $bindableDevices.captureConfig.deviceChannels,
          supportedChannels: bindableDevices.captureConfig.supportedChannels
        ) {
          VStack(alignment: .leading, spacing: 8) {
            HStack {
              Text("Sample Rate")
                .frame(width: 100, alignment: .leading)
              if bindableSettings.resamplerEnabled {
                Picker("", selection: $bindableDevices.captureConfig.sampleRate) {
                  ForEach(bindableDevices.captureRateOptions, id: \.self) { rate in
                    Text(formatRate(rate)).tag(rate)
                  }
                }
                .labelsHidden()
              } else {
                Text(formatRate(bindableDevices.captureConfig.sampleRate))
                  .font(.system(.body, design: .monospaced))
                  .foregroundStyle(.secondary)
              }
            }

            HStack {
              Text("Format")
                .frame(width: 100, alignment: .leading)
              if bindableDevices.captureConfig.supportedFormats.isEmpty {
                Text(bindableDevices.captureConfig.format)
                  .font(.system(.body, design: .monospaced))
                  .foregroundStyle(.secondary)
              } else {
                Picker("", selection: $bindableDevices.captureConfig.format) {
                  ForEach(bindableDevices.captureConfig.supportedFormats, id: \.self) { fmt in
                    Text(fmt).tag(fmt)
                  }
                }
                .labelsHidden()
              }
            }

            Divider()
              .padding(.vertical, 2)

            if DSPEngine.isSwiftEngine {
              Toggle("Bypass DoP Detection", isOn: $bindableDevices.captureConfig.bypassDoP)

              HStack {
                Text("DoP Cutoff")
                  .frame(width: 100, alignment: .leading)
                Picker("", selection: $bindableDevices.captureConfig.dopCutoffHz) {
                  Text("20 kHz").tag(20_000.0)
                  Text("25 kHz").tag(25_000.0)
                  Text("30 kHz").tag(30_000.0)
                  Text("40 kHz").tag(40_000.0)
                  Text("50 kHz").tag(50_000.0)
                }
                .labelsHidden()
                .disabled(bindableDevices.captureConfig.bypassDoP)
              }
              Text("Lower cutoff = higher SINAD; higher cutoff preserves more ultrasonic content")
                .font(.caption)
                .foregroundStyle(.tertiary)
            }

            if !bindableSettings.resamplerEnabled {
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
          devices: bindableDevices.playbackDevices,
          selectedDevice: Binding(
            get: { bindableDevices.playbackConfig.deviceName },
            set: { bindableDevices.playbackConfig.deviceName = $0 }),
          channels: $bindableDevices.playbackConfig.channels,
          deviceChannels: $bindableDevices.playbackConfig.deviceChannels,
          supportedChannels: bindableDevices.playbackConfig.supportedChannels
        ) {
          VStack(alignment: .leading, spacing: 8) {
            HStack {
              Text("Sample Rate")
                .frame(width: 100, alignment: .leading)
              Picker("", selection: $bindableDevices.playbackConfig.sampleRate) {
                ForEach(bindableDevices.playbackRateOptions, id: \.self) { rate in
                  Text(formatRate(rate)).tag(rate)
                }
              }
              .labelsHidden()
            }

            HStack {
              Text("Format")
                .frame(width: 100, alignment: .leading)
              if bindableDevices.playbackConfig.supportedFormats.isEmpty {
                Text(bindableDevices.playbackConfig.format)
                  .font(.system(.body, design: .monospaced))
                  .foregroundStyle(.secondary)
              } else {
                Picker("", selection: $bindableDevices.playbackConfig.format) {
                  ForEach(bindableDevices.playbackConfig.supportedFormats, id: \.self) { fmt in
                    Text(fmt).tag(fmt)
                  }
                }
                .labelsHidden()
              }
            }

            Toggle("Exclusive Mode (Hog)", isOn: $bindableDevices.exclusiveMode)
            Text(
              "Takes exclusive access to the output device, preventing other apps from using it"
            )
            .font(.caption)
            .foregroundStyle(.secondary)

            if DSPEngine.isSwiftEngine {
              let isCapable = [176_400, 352_800, 705_600, 192_000, 384_000, 768_000].contains(
                bindableDevices.playbackConfig.sampleRate)

              Divider()
                .padding(.vertical, 2)

              Toggle("Output DoP (DSD-over-PCM)", isOn: $bindableDevices.playbackConfig.outputDoP)
                .disabled(!isCapable)

              HStack {
                Text("SDM Filter")
                  .frame(width: 100, alignment: .leading)
                Picker("", selection: $bindableDevices.playbackConfig.dopEncoderFilter) {
                  ForEach(SDMFilter.allCases, id: \.self) { filter in
                    Text(filter.rawValue).tag(filter)
                  }
                }
                .labelsHidden()
                .disabled(!bindableDevices.playbackConfig.outputDoP || !isCapable)
              }

              if !isCapable {
                Text(
                  "Sample rate must be a DSD carrier rate (176.4 / 192 / 352.8 / 384 / 705.6 / 768 kHz) to enable DoP output"
                )
                .font(.caption)
                .foregroundStyle(.secondary)
              }
            }
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
              Picker("", selection: $bindableSettings.chunkSize) {
                Text("256 samples").tag(256)
                Text("512 samples").tag(512)
                Text("1024 samples").tag(1024)
                Text("2048 samples").tag(2048)
                Text("4096 samples").tag(4096)
                Text("8192 samples").tag(8192)
                Text("16384 samples").tag(16384)
                Text("32768 samples").tag(32768)
              }
              .labelsHidden()

              Text(latencyText)
                .font(.caption)
                .foregroundStyle(.secondary)
            }

            Toggle("Enable Rate Adjust", isOn: $bindableSettings.enableRateAdjust)
            Text("Compensate for clock drift between capture and playback devices")
              .font(.caption)
              .foregroundStyle(.secondary)

            if !DSPEngine.isSwiftEngine {
              Divider()
                .padding(.vertical, 4)

              HStack {
                Text("Queue Limit")
                  .frame(width: 120, alignment: .leading)
                Stepper(
                  "\(bindableSettings.queuelimit)", value: $bindableSettings.queuelimit, in: 1...32
                )
                .frame(width: 120)
              }

              Toggle("Stop on Rate Change", isOn: $bindableSettings.stopOnRateChange)

              HStack {
                Text("Measure Interval")
                  .frame(width: 120, alignment: .leading)
                Slider(value: $bindableSettings.rateMeasureInterval, in: 0.1...10.0, step: 0.1)
                  .frame(width: 150)
                Text(String(format: "%.1f s", bindableSettings.rateMeasureInterval))
                  .font(.system(.body, design: .monospaced))
              }

              Toggle("Multithreaded", isOn: $bindableSettings.multithreaded)

              if bindableSettings.multithreaded {
                HStack {
                  Text("Worker Threads")
                    .frame(width: 120, alignment: .leading)
                  Stepper(
                    bindableSettings.workerThreads == 0
                      ? "Auto" : "\(bindableSettings.workerThreads)",
                    value: $bindableSettings.workerThreads, in: 0...32
                  )
                  .frame(width: 120)
                }
                .padding(.leading, 16)
              }
            }
          }
          .padding(4)
          .frame(maxWidth: .infinity, alignment: .leading)
        }
      }
      .padding()
    }
    .background(Color(nsColor: .controlBackgroundColor))
  }

  private var latencyText: String {
    String(format: "(%.1f ms latency)", devices.latencyMs)
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
  @Binding var deviceChannels: Int
  let supportedChannels: [Int]
  let extraContent: ExtraContent

  init(
    title: String, icon: String, iconColor: Color, devices: [AudioDevice],
    selectedDevice: Binding<String?>, channels: Binding<Int>, deviceChannels: Binding<Int>,
    supportedChannels: [Int] = [],
    @ViewBuilder extraContent: () -> ExtraContent
  ) {
    self.title = title
    self.icon = icon
    self.iconColor = iconColor
    self.devices = devices
    self._selectedDevice = selectedDevice
    self._channels = channels
    self._deviceChannels = deviceChannels
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

        HStack(spacing: 24) {
          HStack(spacing: 8) {
            Text("Device Channels")
              .frame(width: 110, alignment: .leading)
            if supportedChannels.isEmpty {
              Stepper("\(deviceChannels)", value: $deviceChannels, in: 1...32)
                .frame(width: 100)
            } else {
              Picker("", selection: $deviceChannels) {
                ForEach(supportedChannels, id: \.self) { ch in
                  Text("\(ch)").tag(ch)
                }
              }
              .labelsHidden()
            }
          }

          HStack(spacing: 8) {
            Text("Stream Channels")
              .frame(width: 110, alignment: .leading)
            Stepper("\(channels)", value: $channels, in: 1...deviceChannels)
              .frame(width: 100)
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
