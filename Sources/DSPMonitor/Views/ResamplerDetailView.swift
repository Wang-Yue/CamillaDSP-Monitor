// ResamplerDetailView - Configuration UI for the Sample Rate Converter

import DSPLib
import SwiftUI

struct ResamplerDetailView: View {
  @Environment(AudioSettings.self) var settings
  @Environment(DSPEngineController.self) var dsp
  @Environment(AudioDeviceManager.self) var devices

  var body: some View {
    @Bindable var settings = settings
    ScrollView {
      VStack(alignment: .leading, spacing: 20) {
        HStack {
          Image(systemName: "arrow.triangle.2.circlepath")
            .font(.title2)
            .foregroundStyle(settings.resamplerEnabled ? Color.accentColor : .secondary)
          Text("Sample Rate Converter")
            .font(.title2.bold())
          Spacer()
          Toggle("Enabled", isOn: $settings.resamplerEnabled)
            .onChange(of: settings.resamplerEnabled) { _, _ in dsp.applyConfig() }
        }

        Divider()

        Group {
          GroupBox("Resampler Type") {
            VStack(alignment: .leading, spacing: 12) {
              HStack(spacing: 16) {
                Text("Type")
                  .font(.subheadline)
                  .foregroundStyle(.secondary)
                  .fixedSize()

                Picker("", selection: $settings.resamplerType) {
                  // Filter the segmented picker per active engine:
                  //   Swift engine — only `.synchronous` and `.apple`
                  //                  are implemented natively.
                  //   Rust engine  — `.apple` is unavailable; the
                  //                  rubato-native types (asyncSinc /
                  //                  asyncPoly / synchronous) are.
                  ForEach(
                    ResamplerType.allCases.filter { type in
                      DSPEngine.isSwiftEngine
                        ? (type == .synchronous || type == .apple)
                        : (type != .apple)
                    }
                  ) { type in
                    Text(type.rawValue).tag(type)
                  }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(maxWidth: .infinity, alignment: .leading)
                .frame(minWidth: 400)
                .onChange(of: settings.resamplerType) { _, _ in dsp.applyConfig() }

                Spacer()
              }

              if settings.resamplerType == .asyncSinc {
                HStack(spacing: 16) {
                  Text("Profile")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize()

                  Picker("", selection: $settings.resamplerProfile) {
                    ForEach(ResamplerProfile.allCases) { profile in
                      Text(profile.rawValue).tag(profile)
                    }
                  }
                  .pickerStyle(.segmented)
                  .labelsHidden()
                  .frame(maxWidth: .infinity, alignment: .leading)
                  .frame(minWidth: 400)
                  .onChange(of: settings.resamplerProfile) { _, _ in dsp.applyConfig() }

                  Spacer()
                }
              }

              if settings.resamplerType == .asyncPoly {
                HStack(spacing: 16) {
                  Text("Interp")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize()

                  Picker("", selection: $settings.resamplerInterpolation) {
                    ForEach(ResamplerInterpolation.allCases) { interpolation in
                      Text(interpolation.rawValue).tag(interpolation)
                    }
                  }
                  .pickerStyle(.segmented)
                  .labelsHidden()
                  .frame(maxWidth: .infinity, alignment: .leading)
                  .frame(minWidth: 400)
                  .onChange(of: settings.resamplerInterpolation) { _, _ in dsp.applyConfig() }

                  Spacer()
                }
              }

              if settings.resamplerType == .apple {
                HStack(spacing: 16) {
                  Text("Quality")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize()

                  Picker("", selection: $settings.resamplerAppleQuality) {
                    ForEach(ResamplerAppleQuality.allCases) { quality in
                      Text(quality.rawValue).tag(quality)
                    }
                  }
                  .pickerStyle(.segmented)
                  .labelsHidden()
                  .frame(maxWidth: .infinity, alignment: .leading)
                  .frame(minWidth: 400)
                  .onChange(of: settings.resamplerAppleQuality) { _, _ in dsp.applyConfig() }

                  Spacer()
                }

                HStack(spacing: 16) {
                  Text("Algorithm")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize()

                  Picker("", selection: $settings.resamplerAppleComplexity) {
                    ForEach(ResamplerAppleComplexity.allCases) { complexity in
                      Text(complexity.rawValue).tag(complexity)
                    }
                  }
                  .pickerStyle(.segmented)
                  .labelsHidden()
                  .frame(maxWidth: .infinity, alignment: .leading)
                  .frame(minWidth: 400)
                  .onChange(of: settings.resamplerAppleComplexity) { _, _ in dsp.applyConfig() }

                  Spacer()
                }
              }
            }
            .padding(.vertical, 4)
            .frame(maxWidth: .infinity, alignment: .leading)
          }

          GroupBox("Sample Rates") {
            VStack(alignment: .leading, spacing: 8) {
              HStack(spacing: 16) {
                Text("Capture").foregroundStyle(.secondary).fixedSize()
                Text("\(formatRate(devices.captureConfig.sampleRate))").font(
                  .system(.body, design: .monospaced))
                Spacer()
              }
              HStack(spacing: 16) {
                Text("Playback").foregroundStyle(.secondary).fixedSize()
                Text("\(formatRate(devices.playbackConfig.sampleRate))").font(
                  .system(.body, design: .monospaced))
                Spacer()
              }
              let ratio =
                Double(devices.playbackConfig.sampleRate) / Double(devices.captureConfig.sampleRate)
              Text("Conversion ratio: \(String(format: "%.4f", ratio))").font(.caption)
                .foregroundStyle(.tertiary)
            }
            .padding(.vertical, 4)
            .frame(maxWidth: .infinity, alignment: .leading)
          }

          Text(
            "Resamples audio between capture and playback sample rates. Configure sample rates in the Devices page."
          )
          .font(.caption)
          .foregroundStyle(.secondary)
          .frame(maxWidth: .infinity, alignment: .leading)
        }
        .disabled(!settings.resamplerEnabled)
        .opacity(settings.resamplerEnabled ? 1.0 : 0.5)

        Spacer()
      }
      .padding()
      .frame(maxWidth: .infinity, alignment: .leading)
    }
    .background(Color(nsColor: .controlBackgroundColor))
  }

  private func formatRate(_ rate: Int) -> String {
    if rate >= 1000 {
      return String(format: "%.1f kHz", Double(rate) / 1000.0)
    }
    return "\(rate) Hz"
  }
}
