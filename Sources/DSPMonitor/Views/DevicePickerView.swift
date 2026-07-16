// DevicePickerView - Audio device selection and configuration

import DSPConfig
import DSPLib
import Observation
import SwiftUI

enum ChunkSizeOption: Int, CaseIterable, Identifiable {
  case s256 = 256
  case s512 = 512
  case s1024 = 1024
  case s2048 = 2048
  case s4096 = 4096
  case s8192 = 8192
  case s16384 = 16384
  case s32768 = 32768

  var id: Int { rawValue }
  var label: String { "\(rawValue) samples" }
}

enum DoPCutoffFrequency: Double, CaseIterable, Identifiable {
  case c20k = 20000.0
  case c25k = 25000.0
  case c30k = 30000.0
  case c40k = 40000.0
  case c50k = 50000.0

  var id: Double { rawValue }
  var label: String { "\(Int(rawValue / 1000)) kHz" }
}

struct DevicePickerView: View {
  @Environment(AudioDeviceManager.self) var devices
  @Environment(AudioSettings.self) var settings

  var body: some View {
    @Bindable var bindableDevices = devices
    @Bindable var bindableSettings = settings
    ScrollView {
      VStack(spacing: 20) {
        // Capture device
        GroupBox {
          VStack(alignment: .leading, spacing: 12) {
            Label("Capture (Input)", systemImage: "mic.fill")
              .font(.headline)
              .foregroundStyle(.blue)

            HStack {
              Text("Backend")
                .frame(width: 100, alignment: .leading)
              Picker("", selection: $bindableDevices.captureConfig.backend) {
                ForEach(AudioBackendType.allCases, id: \.self) { type in
                  Text(type.rawValue).tag(type)
                }
              }
              .labelsHidden()
            }

            Divider()

            switch bindableDevices.captureConfig.backend {
            case .coreAudio:
              CoreAudioDeviceSelectionView(
                devices: bindableDevices.captureDevices,
                selectedDevice: $bindableDevices.captureConfig.deviceName,
                channels: $bindableDevices.captureConfig.channels,
                deviceChannels: $bindableDevices.captureConfig.deviceChannels,
                supportedChannels: bindableDevices.captureConfig.supportedChannels,
                sampleRate: $bindableDevices.captureConfig.sampleRate,
                format: $bindableDevices.captureConfig.format,
                supportedRates: bindableDevices.captureRateOptions,
                supportedFormats: bindableDevices.captureConfig.supportedFormats,
                resamplerEnabled: bindableSettings.resamplerEnabled,
                bypassDoP: $bindableDevices.captureConfig.bypassDoP,
                dopCutoffHz: $bindableDevices.captureConfig.dopCutoffHz
              )
            case .rawFile:
              FileSelectionView(
                filename: $bindableDevices.captureConfig.filename,
                format: $bindableDevices.captureConfig.fileFormat,
                isWav: false,
                channels: $bindableDevices.captureConfig.channels,
                skipBytes: $bindableDevices.captureConfig.skipBytes,
                readBytes: $bindableDevices.captureConfig.readBytes,
                extraSamples: $bindableDevices.captureConfig.extraSamples,
                showExtras: true,
                isCapture: true
              )
            case .wavFile:
              FileSelectionView(
                filename: $bindableDevices.captureConfig.filename,
                format: $bindableDevices.captureConfig.fileFormat,
                isWav: true,
                channels: $bindableDevices.captureConfig.channels,
                skipBytes: $bindableDevices.captureConfig.skipBytes,
                readBytes: $bindableDevices.captureConfig.readBytes,
                extraSamples: $bindableDevices.captureConfig.extraSamples,
                showExtras: true,
                isCapture: true
              )
            case .signalGenerator:
              GeneratorSelectionView(
                channels: $bindableDevices.captureConfig.channels,
                genType: $bindableDevices.captureConfig.generatorType,
                freq: $bindableDevices.captureConfig.generatorFreq,
                level: $bindableDevices.captureConfig.generatorLevel
              )
            }
          }
          .padding(4)
          .frame(maxWidth: .infinity, alignment: .leading)
        }

        // Playback device
        GroupBox {
          VStack(alignment: .leading, spacing: 12) {
            Label("Playback (Output)", systemImage: "hifispeaker.2.fill")
              .font(.headline)
              .foregroundStyle(.green)

            HStack {
              Text("Backend")
                .frame(width: 100, alignment: .leading)
              Picker("", selection: $bindableDevices.playbackConfig.backend) {
                Text("CoreAudio").tag(AudioBackendType.coreAudio)
                Text("RawFile").tag(AudioBackendType.rawFile)
              }
              .labelsHidden()
            }

            Divider()

            switch bindableDevices.playbackConfig.backend {
            case .coreAudio:
              CoreAudioPlaybackSelectionView(
                devices: bindableDevices.playbackDevices,
                selectedDevice: $bindableDevices.playbackConfig.deviceName,
                channels: $bindableDevices.playbackConfig.channels,
                deviceChannels: $bindableDevices.playbackConfig.deviceChannels,
                supportedChannels: bindableDevices.playbackConfig.supportedChannels,
                sampleRate: $bindableDevices.playbackConfig.sampleRate,
                format: $bindableDevices.playbackConfig.format,
                supportedRates: bindableDevices.playbackRateOptions,
                supportedFormats: bindableDevices.playbackConfig.supportedFormats,
                exclusiveMode: $bindableDevices.exclusiveMode,
                outputDoP: $bindableDevices.playbackConfig.outputDoP,
                dsdEncoderFilter: $bindableDevices.playbackConfig.dsdEncoderFilter
              )
            case .rawFile:
              FileSelectionView(
                filename: $bindableDevices.playbackConfig.filename,
                format: $bindableDevices.playbackConfig.fileFormat,
                isWav: false,
                channels: $bindableDevices.playbackConfig.channels,
                skipBytes: Self.zeroBinding,
                readBytes: Self.zeroBinding,
                extraSamples: Self.zeroBinding,
                showExtras: false,
                isCapture: false
              )
            case .wavFile, .signalGenerator:
              EmptyView()
            }
          }
          .padding(4)
          .frame(maxWidth: .infinity, alignment: .leading)
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
                ForEach(ChunkSizeOption.allCases) { option in
                  Text(option.label).tag(option.rawValue)
                }
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
          .padding(4)
          .frame(maxWidth: .infinity, alignment: .leading)
        }
      }
      .padding()
    }
    .background(Color(nsColor: .controlBackgroundColor))
  }

  private static let zeroBinding: Binding<Int> = .constant(0)

  private var latencyText: String {
    String(format: "(%.1f ms latency)", devices.latencyMs)
  }
}

// MARK: - Helper Views

struct CoreAudioDeviceSelectionView: View {
  let devices: [AudioDevice]
  @Binding var selectedDevice: String?
  @Binding var channels: Int
  @Binding var deviceChannels: Int
  let supportedChannels: [Int]
  @Binding var sampleRate: Int
  @Binding var format: String
  let supportedRates: [Int]
  let supportedFormats: [String]
  let resamplerEnabled: Bool
  @Binding var bypassDoP: Bool
  @Binding var dopCutoffHz: Double

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
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

      HStack {
        Text("Sample Rate")
          .frame(width: 100, alignment: .leading)
        if resamplerEnabled {
          Picker("", selection: $sampleRate) {
            ForEach(supportedRates, id: \.self) { rate in
              Text(formatRate(rate)).tag(rate)
            }
          }
          .labelsHidden()
        } else {
          Text(formatRate(sampleRate))
            .font(.system(.body, design: .monospaced))
            .foregroundStyle(.secondary)
        }
      }

      HStack {
        Text("Format")
          .frame(width: 100, alignment: .leading)
        if supportedFormats.isEmpty {
          Text(format)
            .font(.system(.body, design: .monospaced))
            .foregroundStyle(.secondary)
        } else {
          Picker("", selection: $format) {
            ForEach(supportedFormats, id: \.self) { fmt in
              Text(fmt).tag(fmt)
            }
          }
          .labelsHidden()
        }
      }

      if !DSPEngine.isRustEngine {
        Divider()
          .padding(.vertical, 2)

        Toggle("Bypass DoP Detection", isOn: $bypassDoP)

        HStack {
          Text("DoP Cutoff")
            .frame(width: 100, alignment: .leading)
          Picker("", selection: $dopCutoffHz) {
            ForEach(DoPCutoffFrequency.allCases) { item in
              Text(item.label).tag(item.rawValue)
            }
          }
          .labelsHidden()
          .disabled(bypassDoP)
        }
        Text("Lower cutoff = higher SINAD; higher cutoff preserves more ultrasonic content")
          .font(.caption)
          .foregroundStyle(.tertiary)
      }
    }
  }

  private func formatRate(_ rate: Int) -> String {
    rate >= 1000 ? String(format: "%.1f kHz", Double(rate) / 1000.0) : "\(rate) Hz"
  }
}

struct CoreAudioPlaybackSelectionView: View {
  let devices: [AudioDevice]
  @Binding var selectedDevice: String?
  @Binding var channels: Int
  @Binding var deviceChannels: Int
  let supportedChannels: [Int]
  @Binding var sampleRate: Int
  @Binding var format: String
  let supportedRates: [Int]
  let supportedFormats: [String]
  @Binding var exclusiveMode: Bool
  @Binding var outputDoP: Bool
  @Binding var dsdEncoderFilter: SDMFilter

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
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

      HStack {
        Text("Sample Rate")
          .frame(width: 100, alignment: .leading)
        Picker("", selection: $sampleRate) {
          ForEach(supportedRates, id: \.self) { rate in
            Text(formatRate(rate)).tag(rate)
          }
        }
        .labelsHidden()
      }

      HStack {
        Text("Format")
          .frame(width: 100, alignment: .leading)
        if supportedFormats.isEmpty {
          Text(format)
            .font(.system(.body, design: .monospaced))
            .foregroundStyle(.secondary)
        } else {
          Picker("", selection: $format) {
            ForEach(supportedFormats, id: \.self) { fmt in
              Text(fmt).tag(fmt)
            }
          }
          .labelsHidden()
        }
      }

      Toggle("Exclusive Mode (Hog)", isOn: $exclusiveMode)
      Text("Takes exclusive access to the output device, preventing other apps from using it")
        .font(.caption)
        .foregroundStyle(.secondary)

      if !DSPEngine.isRustEngine {
        let isCapable = [176_400, 352_800, 705_600, 192_000, 384_000, 768_000].contains(sampleRate)

        Divider()
          .padding(.vertical, 2)

        Toggle("Output DoP (DSD-over-PCM)", isOn: $outputDoP)
          .disabled(!isCapable)

        HStack {
          Text("SDM Filter")
            .frame(width: 100, alignment: .leading)
          Picker("", selection: $dsdEncoderFilter) {
            ForEach(SDMFilter.allCases, id: \.self) { filter in
              Text(filter.rawValue).tag(filter)
            }
          }
          .labelsHidden()
          .disabled(!outputDoP || !isCapable)
        }

        if !isCapable {
          Text("Sample rate must be a DSD carrier rate to enable DoP output")
            .font(.caption)
            .foregroundStyle(.secondary)
        }
      }
    }
  }

  private func formatRate(_ rate: Int) -> String {
    rate >= 1000 ? String(format: "%.1f kHz", Double(rate) / 1000.0) : "\(rate) Hz"
  }
}

struct FileSelectionView: View {
  @Binding var filename: String
  @Binding var format: String
  let isWav: Bool
  @Binding var channels: Int
  @Binding var skipBytes: Int
  @Binding var readBytes: Int
  @Binding var extraSamples: Int
  let showExtras: Bool
  let isCapture: Bool

  let formats = [
    "S16_LE", "S24_3_LE", "S24_4_RJ_LE", "S24_4_LJ_LE", "S32_LE", "F32_LE", "F64_LE",
  ]

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Text("File Path")
          .frame(width: 100, alignment: .leading)
        TextField(isWav ? "e.g. /path/to/audio.wav" : "e.g. /path/to/audio.raw", text: $filename)
          .textFieldStyle(.roundedBorder)
      }

      if isCapture && isWav {
        Text("Sample rate, format, and channel count are parsed from the file header")
          .font(.caption)
          .foregroundStyle(.secondary)
      } else {
        HStack {
          Text("Format")
            .frame(width: 100, alignment: .leading)
          Picker("", selection: $format) {
            ForEach(formats, id: \.self) { fmt in
              Text(fmt).tag(fmt)
            }
          }
          .labelsHidden()
        }

        HStack {
          Text("Channels")
            .frame(width: 100, alignment: .leading)
          Stepper("\(channels)", value: $channels, in: 1...32)
            .frame(width: 100)
        }
      }

      if showExtras {
        Divider()
          .padding(.vertical, 2)

        HStack {
          Text("Skip Bytes")
            .frame(width: 100, alignment: .leading)
          TextField("0", value: $skipBytes, format: .number)
            .textFieldStyle(.roundedBorder)
            .frame(width: 100)
        }

        HStack {
          Text("Read Bytes")
            .frame(width: 100, alignment: .leading)
          TextField("0 (All)", value: $readBytes, format: .number)
            .textFieldStyle(.roundedBorder)
            .frame(width: 100)
        }

        HStack {
          Text("Extra Samples")
            .frame(width: 100, alignment: .leading)
          Stepper("\(extraSamples)", value: $extraSamples, in: 0...1_000_000)
            .frame(width: 120)
        }
      }
    }
  }
}

struct GeneratorSelectionView: View {
  @Binding var channels: Int
  @Binding var genType: String
  @Binding var freq: Double
  @Binding var level: Double

  let types = ["Sine", "Square", "WhiteNoise"]

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Text("Generator")
          .frame(width: 100, alignment: .leading)
        Picker("", selection: $genType) {
          ForEach(types, id: \.self) { type in
            Text(type).tag(type)
          }
        }
        .labelsHidden()
      }

      HStack {
        Text("Channels")
          .frame(width: 100, alignment: .leading)
        Stepper("\(channels)", value: $channels, in: 1...32)
          .frame(width: 100)
      }

      HStack {
        Text("Freq (Hz)")
          .frame(width: 100, alignment: .leading)
        TextField("1000", value: $freq, format: .number)
          .textFieldStyle(.roundedBorder)
          .frame(width: 100)
        Slider(value: $freq, in: 1.0...20000.0, step: 1.0)
      }
      .disabled(genType == "WhiteNoise")

      HStack {
        Text("Level (dB)")
          .frame(width: 100, alignment: .leading)
        TextField("-6", value: $level, format: .number)
          .textFieldStyle(.roundedBorder)
          .frame(width: 100)
        Slider(value: $level, in: -100.0...0.0, step: 0.5)
      }
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
