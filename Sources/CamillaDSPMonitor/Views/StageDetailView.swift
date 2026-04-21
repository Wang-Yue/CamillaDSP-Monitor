// StageDetailView - Configuration UI for each pipeline stage

import CamillaDSPLib
import SwiftUI

struct StageDetailView: View {
  let stageIndex: Int
  @EnvironmentObject var pipeline: PipelineStore

  var body: some View {
    if stageIndex < pipeline.stages.count {
      StageDetailContent(stage: pipeline.stages[stageIndex])
    } else {
      Text("Stage not found")
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(nsColor: .controlBackgroundColor))
    }
  }
}

private struct StageDetailContent: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var dsp: DSPEngineController

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 20) {
        HStack {
          Image(systemName: stage.type.icon)
            .font(.title2)
            .foregroundStyle(stage.isEnabled ? Color.accentColor : .secondary)
          Text(stage.name)
            .font(.title2.bold())
          Spacer()
          Toggle("Enabled", isOn: $stage.isEnabled)
            .onChange(of: stage.isEnabled) { _, _ in dsp.applyConfig() }
        }

        Divider()

        Group {
          switch stage.type {
          case .balance: BalanceOptions(stage: stage)
          case .width: WidthOptions(stage: stage)
          case .msProc: MSProcDescription()
          case .phaseInvert: PhaseInvertOptions(stage: stage)
          case .crossfeed: CrossfeedOptions(stage: stage)
          case .eq: EQOptions(stage: stage)
          case .loudness: LoudnessOptions(stage: stage)
          case .emphasis: EmphasisOptions(stage: stage)
          case .dcProtection: DCProtectionDescription()
          }
        }
        .disabled(!stage.isEnabled)
        .opacity(stage.isEnabled ? 1.0 : 0.5)

        Spacer()
      }
      .padding()
      .frame(maxWidth: .infinity, alignment: .leading)
    }
    .background(Color(nsColor: .controlBackgroundColor))
  }
}

// MARK: - Balance

struct BalanceOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var dsp: DSPEngineController

  var body: some View {
    GroupBox("Balance") {
      VStack(spacing: 12) {
        HStack {
          Text("L")
            .font(.system(.body, design: .monospaced).bold())
            .foregroundStyle(.secondary)
          Slider(value: $stage.balancePosition, in: -1.0...1.0, step: 0.01)
            .onChange(of: stage.balancePosition) { _, _ in dsp.applyConfig() }
          Text("R")
            .font(.system(.body, design: .monospaced).bold())
            .foregroundStyle(.secondary)
        }

        HStack {
          Text("Left: \(stage.balanceLeftPercent)%")
            .font(.system(.caption, design: .monospaced))
            .foregroundStyle(.secondary)
            .frame(maxWidth: .infinity, alignment: .leading)
          Button("Center") {
            stage.balancePosition = 0.0
            dsp.applyConfig()
          }
          .controlSize(.small)
          Text("Right: \(stage.balanceRightPercent)%")
            .font(.system(.caption, design: .monospaced))
            .foregroundStyle(.secondary)
            .frame(maxWidth: .infinity, alignment: .trailing)
        }
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - Width

struct WidthOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var dsp: DSPEngineController

  var body: some View {
    GroupBox("Stereo Width") {
      VStack(spacing: 12) {
        HStack {
          Text("Swapped")
            .font(.caption)
            .foregroundStyle(.secondary)
          Slider(value: $stage.widthAmount, in: -1.0...2.0, step: 0.01)
            .onChange(of: stage.widthAmount) { _, _ in dsp.applyConfig() }
          Text("Wide")
            .font(.caption)
            .foregroundStyle(.secondary)
        }

        HStack {
          Text("\(stage.widthPercent)%")
            .font(.system(.title3, design: .monospaced).bold())
          Spacer()
          HStack(spacing: 12) {
            Button("-100%") {
              stage.widthAmount = -1.0
              dsp.applyConfig()
            }
            .controlSize(.small)
            Button("Mono") {
              stage.widthAmount = 0.0
              dsp.applyConfig()
            }
            .controlSize(.small)
            Button("100%") {
              stage.widthAmount = 1.0
              dsp.applyConfig()
            }
            .controlSize(.small)
          }
        }

        Text(stage.widthDescription)
          .font(.caption)
          .foregroundStyle(.secondary)
          .frame(maxWidth: .infinity, alignment: .leading)
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - M/S Proc

struct MSProcDescription: View {
  var body: some View {
    GroupBox("Mid/Side Processing") {
      Text("Encodes stereo to Mid (L+R) and Side (L-R) signals at -6.02 dB")
        .font(.caption)
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.vertical, 4)
    }
  }
}

// MARK: - Phase Invert

struct PhaseInvertOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var dsp: DSPEngineController

  var body: some View {
    GroupBox("Phase Inversion") {
      VStack(alignment: .leading, spacing: 8) {
        HStack(spacing: 16) {
          Text("Channel")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize()

          Picker("", selection: $stage.phaseInvertMode) {
            Text("Left").tag(PhaseInvertMode.left)
            Text("Right").tag(PhaseInvertMode.right)
            Text("Both").tag(PhaseInvertMode.both)
          }
          .pickerStyle(.segmented)
          .labelsHidden()
          .frame(maxWidth: .infinity, alignment: .leading)
          .frame(minWidth: 300)
          .onChange(of: stage.phaseInvertMode) { _, _ in dsp.applyConfig() }

          Spacer()
        }

        Text(stage.phaseInvertMode.description)
          .font(.caption)
          .foregroundStyle(.secondary)
          .frame(maxWidth: .infinity, alignment: .leading)
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - Crossfeed

struct CrossfeedOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var dsp: DSPEngineController

  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      GroupBox("Preset") {
        VStack(alignment: .leading, spacing: 10) {
          HStack(spacing: 16) {
            Text("Level")
              .font(.subheadline)
              .foregroundStyle(.secondary)
              .fixedSize()

            Picker("", selection: $stage.crossfeedLevel) {
              Text("L1").tag(CrossfeedLevel.l1)
              Text("L2").tag(CrossfeedLevel.l2)
              Text("L3").tag(CrossfeedLevel.l3)
              Text("L4").tag(CrossfeedLevel.l4)
              Text("L5").tag(CrossfeedLevel.l5)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .disabled(stage.cxCustomEnabled)
            .frame(maxWidth: .infinity, alignment: .leading)
            .frame(minWidth: 400)
            .onChange(of: stage.crossfeedLevel) { _, _ in dsp.applyConfig() }

            Spacer()
          }

          if let preset = PipelineStage.crossfeedPresets[stage.crossfeedLevel] {
            Text(
              "Fc = \(String(format: "%.0f", preset.fc)) Hz, Level = \(String(format: "%.1f", preset.db)) dB — \(stage.crossfeedLevel.description)"
            )
            .font(.caption)
            .foregroundStyle(.tertiary)
          }
        }
        .padding(.vertical, 4)
        .frame(maxWidth: .infinity, alignment: .leading)
      }
      .opacity(stage.cxCustomEnabled ? 0.5 : 1.0)

      GroupBox {
        VStack(alignment: .leading, spacing: 12) {
          Toggle("Custom Parameters", isOn: $stage.cxCustomEnabled)
            .onChange(of: stage.cxCustomEnabled) { _, enabled in
              if enabled {
                if let preset = PipelineStage.crossfeedPresets[stage.crossfeedLevel] {
                  stage.cxFc = preset.fc
                  stage.cxDb = preset.db
                }
              }
              dsp.applyConfig()
            }

          if stage.cxCustomEnabled {
            VStack(spacing: 12) {
              HStack(spacing: 16) {
                Text("Fc (Hz)")
                  .font(.subheadline)
                  .foregroundStyle(.secondary)
                  .fixedSize()
                Slider(value: $stage.cxFc, in: 300...1200, step: 10)
                  .frame(maxWidth: .infinity)
                  .frame(minWidth: 300)
                  .onChange(of: stage.cxFc) { _, _ in dsp.applyConfig() }
                Text("\(String(format: "%.0f", stage.cxFc))")
                  .font(.system(.body, design: .monospaced))
                  .fixedSize()
                Spacer()
              }
              HStack(spacing: 16) {
                Text("Level (dB)")
                  .font(.subheadline)
                  .foregroundStyle(.secondary)
                  .fixedSize()
                Slider(value: $stage.cxDb, in: 1...20, step: 0.5)
                  .frame(maxWidth: .infinity)
                  .frame(minWidth: 300)
                  .onChange(of: stage.cxDb) { _, _ in dsp.applyConfig() }
                Text("\(String(format: "%.1f", stage.cxDb))")
                  .font(.system(.body, design: .monospaced))
                  .fixedSize()
                Spacer()
              }
            }
            .transition(.opacity)
          }
        }
        .padding(.vertical, 4)
        .frame(maxWidth: .infinity, alignment: .leading)
      }

      let cx = stage.activeCrossfeedParams
      GroupBox("Computed Filter Parameters") {
        Grid(alignment: .leading, horizontalSpacing: 24, verticalSpacing: 8) {
          GridRow {
            Text("Lowshelf").foregroundStyle(.secondary).bold()
            Text("\(String(format: "%.1f Hz", cx.hiFreq))").font(
              .system(.body, design: .monospaced))
            Text("\(String(format: "%.2f dB", cx.hiGain))").font(
              .system(.body, design: .monospaced))
            Text("Q 0.5").font(.system(.body, design: .monospaced)).foregroundStyle(.tertiary)
          }
          GridRow {
            Text("Lowpass").foregroundStyle(.secondary).bold()
            Text("\(String(format: "%.0f Hz", cx.loFreq))").font(
              .system(.body, design: .monospaced))
            Text("1st order").font(.caption).foregroundStyle(.tertiary)
            Text("")
          }
          GridRow {
            Text("Cross gain").foregroundStyle(.secondary).bold()
            Text("\(String(format: "%.2f dB", cx.loGain))").font(
              .system(.body, design: .monospaced))
            Text("")
            Text("")
          }
        }
        .font(.subheadline)
        .padding(.vertical, 4)
        .frame(maxWidth: .infinity, alignment: .leading)
      }
    }
  }
}

// MARK: - EQ

struct EQOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var dsp: DSPEngineController
  @EnvironmentObject var pipeline: PipelineStore
  @EnvironmentObject var devices: AudioDeviceManager

  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      GroupBox("Channel Mode") {
        HStack(spacing: 16) {
          Text("Mode")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize()
          Picker("", selection: $stage.eqChannelMode) {
            ForEach(EQChannelMode.allCases) { mode in Text(mode.rawValue).tag(mode) }
          }
          .pickerStyle(.segmented)
          .labelsHidden()
          .frame(maxWidth: 400)
          .onChange(of: stage.eqChannelMode) { _, _ in dsp.applyConfig() }

          Spacer()
        }
        .padding(.vertical, 4)
        .frame(maxWidth: .infinity, alignment: .leading)
      }

      if !pipeline.eqPresets.isEmpty {
        switch stage.eqChannelMode {
        case .same:
          GroupBox("EQ Preset") {
            VStack(alignment: .leading, spacing: 12) {
              HStack(spacing: 16) {
                Text("Preset")
                  .font(.subheadline)
                  .foregroundStyle(.secondary)
                  .fixedSize()
                EQPresetPicker(selectedID: $stage.eqPresetID, presets: pipeline.eqPresets)
                  .frame(maxWidth: 400)
                  .onChange(of: stage.eqPresetID) { _, _ in dsp.applyConfig() }
                Spacer()
              }

              if let preset = pipeline.eqPresets.first(where: { $0.id == stage.eqPresetID }) {
                EQSummaryCard(
                  title: "Combined L/R", preset: preset,
                  sampleRate: devices.captureConfig.sampleRate)
              }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.vertical, 4)
          }
        case .separate:
          VStack(spacing: 12) {
            GroupBox("Left Channel") {
              VStack(alignment: .leading, spacing: 12) {
                HStack(spacing: 16) {
                  Text("Preset")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize()
                  EQPresetPicker(selectedID: $stage.eqLeftPresetID, presets: pipeline.eqPresets)
                    .frame(maxWidth: 400)
                    .onChange(of: stage.eqLeftPresetID) { _, _ in dsp.applyConfig() }
                  Spacer()
                }

                if let lPreset = pipeline.eqPresets.first(where: { $0.id == stage.eqLeftPresetID })
                {
                  EQSummaryCard(
                    title: "Left", preset: lPreset,
                    sampleRate: devices.captureConfig.sampleRate)
                }
              }
              .frame(maxWidth: .infinity, alignment: .leading)
              .padding(.vertical, 4)
            }

            GroupBox("Right Channel") {
              VStack(alignment: .leading, spacing: 12) {
                HStack(spacing: 16) {
                  Text("Preset")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize()
                  EQPresetPicker(
                    selectedID: $stage.eqRightPresetID, presets: pipeline.eqPresets)
                    .frame(maxWidth: 400)
                    .onChange(of: stage.eqRightPresetID) { _, _ in dsp.applyConfig() }
                  Spacer()
                }

                if let rPreset = pipeline.eqPresets.first(where: {
                  $0.id == stage.eqRightPresetID
                }) {
                  EQSummaryCard(
                    title: "Right", preset: rPreset,
                    sampleRate: devices.captureConfig.sampleRate)
                }
              }
              .frame(maxWidth: .infinity, alignment: .leading)
              .padding(.vertical, 4)
            }
          }
        }
      }
    }
  }
}

struct EQSummaryCard: View {
  let title: String
  @ObservedObject var preset: EQPreset
  let sampleRate: Int

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack {
        Text("Preamp Gain").foregroundStyle(.secondary)
        Spacer()
        Text("\(String(format: "%+.1f dB", preset.preampGain))").font(
          .system(.body, design: .monospaced))
      }
      EQFrequencyResponseView(
        preset: preset, selectedBandID: .constant(nil), sampleRate: sampleRate
      )
      .frame(height: 150)
      .padding(.horizontal, 16)
      .allowsHitTesting(false)
    }
    .frame(maxWidth: .infinity)
  }
}

struct EQPresetPicker: View {
  @Binding var selectedID: UUID?
  let presets: [EQPreset]
  var body: some View {
    Picker("", selection: $selectedID) {
      Text("None").tag(nil as UUID?)
      ForEach(presets) { preset in Text(preset.name).tag(preset.id as UUID?) }
    }
    .labelsHidden()
  }
}

// MARK: - Loudness

struct LoudnessOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var dsp: DSPEngineController

  var body: some View {
    GroupBox("Loudness Compensation") {
      VStack(alignment: .leading, spacing: 12) {
        HStack(spacing: 16) {
          Text("Reference Level")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize()
          Slider(value: $stage.loudnessReference, in: -50...20, step: 1)
            .frame(maxWidth: .infinity)
            .frame(minWidth: 200)
            .onChange(of: stage.loudnessReference) { _, _ in dsp.applyConfig() }
          Text("\(String(format: "%.0f dB", stage.loudnessReference))").font(
            .system(.body, design: .monospaced)
          ).fixedSize()
          Spacer()
        }
        HStack(spacing: 16) {
          Text("Low Boost")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize()
          Slider(value: $stage.loudnessLowBoost, in: 0...15, step: 0.5)
            .frame(maxWidth: .infinity)
            .frame(minWidth: 200)
            .onChange(of: stage.loudnessLowBoost) { _, _ in dsp.applyConfig() }
          Text("\(String(format: "%.1f dB", stage.loudnessLowBoost))").font(
            .system(.body, design: .monospaced)
          ).fixedSize()
          Spacer()
        }
        HStack(spacing: 16) {
          Text("High Boost")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize()
          Slider(value: $stage.loudnessHighBoost, in: 0...15, step: 0.5)
            .frame(maxWidth: .infinity)
            .frame(minWidth: 200)
            .onChange(of: stage.loudnessHighBoost) { _, _ in dsp.applyConfig() }
          Text("\(String(format: "%.1f dB", stage.loudnessHighBoost))").font(
            .system(.body, design: .monospaced)
          ).fixedSize()
          Spacer()
        }
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      .padding(.vertical, 4)
    }
  }
}

// MARK: - Emphasis

struct EmphasisOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var dsp: DSPEngineController

  var body: some View {
    GroupBox("Emphasis") {
      VStack(alignment: .leading, spacing: 8) {
        HStack(spacing: 16) {
          Text("Mode")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize()

          Picker("", selection: $stage.emphasisMode) {
            Text("De-Emphasis").tag(EmphasisMode.deEmphasis)
            Text("Pre-Emphasis").tag(EmphasisMode.preEmphasis)
          }
          .pickerStyle(.segmented)
          .labelsHidden()
          .frame(maxWidth: .infinity, alignment: .leading)
          .frame(minWidth: 300)
          .onChange(of: stage.emphasisMode) { _, _ in dsp.applyConfig() }

          Spacer()
        }

        Text(stage.emphasisMode.description)
          .font(.caption)
          .foregroundStyle(.secondary)
          .frame(maxWidth: .infinity, alignment: .leading)
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - DC Protection

struct DCProtectionDescription: View {
  var body: some View {
    GroupBox("DC Protection") {
      Text("First-order highpass at 7 Hz — removes DC offset and subsonic content")
        .font(.caption)
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.vertical, 4)
    }
  }
}

// MARK: - Resampler Detail View

struct ResamplerDetailView: View {
  @EnvironmentObject var settings: AudioSettings
  @EnvironmentObject var dsp: DSPEngineController
  @EnvironmentObject var devices: AudioDeviceManager

  var body: some View {
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
                  ForEach(ResamplerType.allCases) { type in
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

              if settings.resamplerType == .asyncSinc || settings.resamplerType == .synchronous {
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
}
