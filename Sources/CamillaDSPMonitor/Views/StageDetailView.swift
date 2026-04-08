// StageDetailView - Configuration UI for each pipeline stage

import CamillaDSPLib
import SwiftUI

struct StageDetailView: View {
  let stageIndex: Int
  @EnvironmentObject var appState: AppState

  var body: some View {
    if stageIndex < appState.stages.count {
      StageDetailContent(stage: appState.stages[stageIndex])
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
  @EnvironmentObject var appState: AppState

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
            .onChange(of: stage.isEnabled) { _, _ in appState.applyConfig() }
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
    }
    .background(Color(nsColor: .controlBackgroundColor))
  }
}

// MARK: - Balance

struct BalanceOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var appState: AppState

  var body: some View {
    GroupBox("Balance") {
      VStack(spacing: 12) {
        HStack {
          Text("L")
            .font(.system(.body, design: .monospaced).bold())
            .foregroundStyle(.secondary)
          Slider(value: $stage.balancePosition, in: -1.0...1.0, step: 0.01)
            .onChange(of: stage.balancePosition) { _, _ in appState.applyConfig() }
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
            appState.applyConfig()
          }
          .controlSize(.small)
          Text("Right: \(stage.balanceRightPercent)%")
            .font(.system(.caption, design: .monospaced))
            .foregroundStyle(.secondary)
            .frame(maxWidth: .infinity, alignment: .trailing)
        }
      }
    }
  }
}

// MARK: - Section 0: Width

struct WidthOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var appState: AppState

  var body: some View {
    GroupBox("Stereo Width") {
      VStack(spacing: 12) {
        HStack {
          Text("Swapped")
            .font(.caption)
            .foregroundStyle(.secondary)
          Slider(value: $stage.widthAmount, in: -1.0...2.0, step: 0.01)
            .onChange(of: stage.widthAmount) { _, _ in appState.applyConfig() }
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
              appState.applyConfig()
            }
            .controlSize(.small)
            Button("Mono") {
              stage.widthAmount = 0.0
              appState.applyConfig()
            }
            .controlSize(.small)
            Button("100%") {
              stage.widthAmount = 1.0
              appState.applyConfig()
            }
            .controlSize(.small)
          }
        }

        Text(stage.widthDescription)
          .font(.caption)
          .foregroundStyle(.secondary)
      }
    }
  }
}

// MARK: - Section 1: M/S Proc

struct MSProcDescription: View {
  var body: some View {
    GroupBox("Mid/Side Processing") {
      Text("Encodes stereo to Mid (L+R) and Side (L-R) signals at -6.02 dB")
        .font(.caption)
        .foregroundStyle(.secondary)
    }
  }
}

// MARK: - Section 2: Phase Invert

struct PhaseInvertOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var appState: AppState

  var body: some View {
    GroupBox("Phase Inversion") {
      Picker("Channel", selection: $stage.phaseInvertMode) {
        Text("Left").tag(PhaseInvertMode.left)
        Text("Right").tag(PhaseInvertMode.right)
        Text("Both").tag(PhaseInvertMode.both)
      }
      .pickerStyle(.segmented)
      .onChange(of: stage.phaseInvertMode) { _, _ in appState.applyConfig() }

      Text(stage.phaseInvertMode.description)
        .font(.caption)
        .foregroundStyle(.secondary)
        .padding(.top, 4)
    }
  }
}

// MARK: - Section 3: Crossfeed

struct CrossfeedOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var appState: AppState

  var body: some View {
    GroupBox("Preset") {
      Picker("Level", selection: $stage.crossfeedLevel) {
        Text("L1").tag(CrossfeedLevel.l1)
        Text("L2").tag(CrossfeedLevel.l2)
        Text("L3").tag(CrossfeedLevel.l3)
        Text("L4").tag(CrossfeedLevel.l4)
        Text("L5").tag(CrossfeedLevel.l5)
      }
      .pickerStyle(.segmented)
      .disabled(stage.cxCustomEnabled)
      .onChange(of: stage.crossfeedLevel) { _, _ in appState.applyConfig() }

      if let preset = PipelineStage.crossfeedPresets[stage.crossfeedLevel] {
        Text(
          "Fc = \(String(format: "%.0f", preset.fc)) Hz, Level = \(String(format: "%.1f", preset.db)) dB — \(stage.crossfeedLevel.description)"
        )
        .font(.caption)
        .foregroundStyle(.tertiary)
        .padding(.top, 2)
      }
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
            appState.applyConfig()
          }

        if stage.cxCustomEnabled {
          HStack {
            Text("Fc (Hz)").frame(width: 90, alignment: .leading)
            Slider(value: $stage.cxFc, in: 300...1200, step: 10)
              .onChange(of: stage.cxFc) { _, _ in appState.applyConfig() }
            Text(String(format: "%.0f", stage.cxFc)).font(.system(.body, design: .monospaced))
              .frame(width: 55, alignment: .trailing)
          }
          HStack {
            Text("Level (dB)").frame(width: 90, alignment: .leading)
            Slider(value: $stage.cxDb, in: 1...20, step: 0.5)
              .onChange(of: stage.cxDb) { _, _ in appState.applyConfig() }
            Text(String(format: "%.1f", stage.cxDb)).font(.system(.body, design: .monospaced))
              .frame(width: 55, alignment: .trailing)
          }
        }
      }
    }

    let cx = stage.activeCrossfeedParams
    GroupBox("Computed Filter Parameters") {
      Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 4) {
        GridRow {
          Text("Lowshelf").foregroundStyle(.secondary)
          Text(String(format: "%.1f Hz", cx.hiFreq)).font(.system(.body, design: .monospaced))
          Text(String(format: "%.2f dB", cx.hiGain)).font(.system(.body, design: .monospaced))
          Text("Q 0.5").font(.system(.body, design: .monospaced))
        }
        GridRow {
          Text("Lowpass").foregroundStyle(.secondary)
          Text(String(format: "%.0f Hz", cx.loFreq)).font(.system(.body, design: .monospaced))
          Text("1st order").font(.caption).foregroundStyle(.tertiary)
          Text("")
        }
        GridRow {
          Text("Cross gain").foregroundStyle(.secondary)
          Text(String(format: "%.2f dB", cx.loGain)).font(.system(.body, design: .monospaced))
          Text("")
          Text("")
        }
      }
      .font(.caption)
    }
  }

}

// MARK: - Section 4: EQ

struct EQOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var appState: AppState

  var body: some View {
    GroupBox("Channel Mode") {
      Picker("Mode", selection: $stage.eqChannelMode) {
        ForEach(EQChannelMode.allCases) { mode in Text(mode.rawValue).tag(mode) }
      }
      .pickerStyle(.segmented)
      .onChange(of: stage.eqChannelMode) { _, _ in appState.applyConfig() }
    }

    if !appState.eqPresets.isEmpty {
      switch stage.eqChannelMode {
      case .same:
        GroupBox("EQ Preset") {
          VStack(alignment: .leading, spacing: 12) {
            EQPresetPicker(
              selectedID: $stage.eqPresetID, label: "Preset", presets: appState.eqPresets
            )
            .onChange(of: stage.eqPresetID) { _, _ in appState.applyConfig() }

            if let preset = appState.eqPresets.first(where: { $0.id == stage.eqPresetID }) {
              EQSummaryCard(title: "Combined L/R", preset: preset, sampleRate: appState.sampleRate)
            }
          }
        }
      case .separate:
        VStack(spacing: 12) {
          GroupBox("Left Channel") {
            VStack(alignment: .leading, spacing: 12) {
              EQPresetPicker(
                selectedID: $stage.eqLeftPresetID, label: "Left Preset", presets: appState.eqPresets
              )
              .onChange(of: stage.eqLeftPresetID) { _, _ in appState.applyConfig() }

              if let lPreset = appState.eqPresets.first(where: { $0.id == stage.eqLeftPresetID }) {
                EQSummaryCard(title: "Left", preset: lPreset, sampleRate: appState.sampleRate)
              }
            }
          }

          GroupBox("Right Channel") {
            VStack(alignment: .leading, spacing: 12) {
              EQPresetPicker(
                selectedID: $stage.eqRightPresetID, label: "Right Preset",
                presets: appState.eqPresets
              )
              .onChange(of: stage.eqRightPresetID) { _, _ in appState.applyConfig() }

              if let rPreset = appState.eqPresets.first(where: { $0.id == stage.eqRightPresetID }) {
                EQSummaryCard(title: "Right", preset: rPreset, sampleRate: appState.sampleRate)
              }
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
        Text(String(format: "%+.1f dB", preset.preampGain)).font(
          .system(.body, design: .monospaced))
      }
      EQFrequencyResponseView(
        preset: preset, selectedBandID: .constant(nil), sampleRate: sampleRate
      )
      .frame(height: 150).allowsHitTesting(false)
    }
  }
}

struct EQPresetPicker: View {
  @Binding var selectedID: UUID?
  let label: String
  let presets: [EQPreset]
  var body: some View {
    Picker(label, selection: $selectedID) {
      Text("None").tag(nil as UUID?)
      ForEach(presets) { preset in Text(preset.name).tag(preset.id as UUID?) }
    }
  }
}

// MARK: - Section 5: Loudness

struct LoudnessOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var appState: AppState

  var body: some View {
    GroupBox("Loudness Compensation") {
      VStack(alignment: .leading, spacing: 12) {
        HStack {
          Text("Reference Level").frame(width: 110, alignment: .leading)
          Slider(value: $stage.loudnessReference, in: -50...20, step: 1)
            .onChange(of: stage.loudnessReference) { _, _ in appState.applyConfig() }
          Text(String(format: "%.0f dB", stage.loudnessReference)).font(
            .system(.body, design: .monospaced)
          ).frame(width: 55, alignment: .trailing)
        }
        HStack {
          Text("Low Boost").frame(width: 110, alignment: .leading)
          Slider(value: $stage.loudnessLowBoost, in: 0...15, step: 0.5)
            .onChange(of: stage.loudnessLowBoost) { _, _ in appState.applyConfig() }
          Text(String(format: "%.1f dB", stage.loudnessLowBoost)).font(
            .system(.body, design: .monospaced)
          ).frame(width: 55, alignment: .trailing)
        }
        HStack {
          Text("High Boost").frame(width: 110, alignment: .leading)
          Slider(value: $stage.loudnessHighBoost, in: 0...15, step: 0.5)
            .onChange(of: stage.loudnessHighBoost) { _, _ in appState.applyConfig() }
          Text(String(format: "%.1f dB", stage.loudnessHighBoost)).font(
            .system(.body, design: .monospaced)
          ).frame(width: 55, alignment: .trailing)
        }
      }
    }
  }
}

// MARK: - Section 6: Emphasis

struct EmphasisOptions: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var appState: AppState

  var body: some View {
    GroupBox("Emphasis") {
      Picker("Mode", selection: $stage.emphasisMode) {
        Text("De-Emphasis").tag(EmphasisMode.deEmphasis)
        Text("Pre-Emphasis").tag(EmphasisMode.preEmphasis)
      }
      .pickerStyle(.segmented)
      .onChange(of: stage.emphasisMode) { _, _ in appState.applyConfig() }

      Text(stage.emphasisMode.description)
        .font(.caption)
        .foregroundStyle(.secondary)
        .padding(.top, 4)
    }
  }
}

// MARK: - Section 7: DC Protection

struct DCProtectionDescription: View {
  var body: some View {
    GroupBox("DC Protection") {
      Text("First-order highpass at 7 Hz — removes DC offset and subsonic content")
        .font(.caption)
        .foregroundStyle(.secondary)
    }
  }
}

// MARK: - Resampler Detail View

struct ResamplerDetailView: View {
  @EnvironmentObject var appState: AppState

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 20) {
        HStack {
          Image(systemName: "arrow.triangle.2.circlepath")
            .font(.title2)
            .foregroundStyle(appState.resamplerEnabled ? Color.accentColor : .secondary)
          Text("Sample Rate Converter")
            .font(.title2.bold())
          Spacer()
          Toggle("Enabled", isOn: $appState.resamplerEnabled)
        }

        Divider()

        Group {
          GroupBox("Resampler Type") {
            VStack(alignment: .leading, spacing: 12) {
              Picker("Type", selection: $appState.resamplerType) {
                Text("Async Sinc (highest quality)").tag(ResamplerType.asyncSinc)
                Text("Async Polynomial (lower latency)").tag(ResamplerType.asyncPoly)
                Text("Synchronous (fixed ratio)").tag(ResamplerType.synchronous)
              }
              .labelsHidden()

              if appState.resamplerType == .asyncSinc {
                Picker("Quality Profile", selection: $appState.resamplerProfile) {
                  Text("Very Fast").tag(ResamplerProfile.veryFast)
                  Text("Fast").tag(ResamplerProfile.fast)
                  Text("Balanced").tag(ResamplerProfile.balanced)
                  Text("Accurate").tag(ResamplerProfile.accurate)
                }
                .pickerStyle(.segmented)
              }
            }
          }

          GroupBox("Sample Rates") {
            VStack(alignment: .leading, spacing: 8) {
              HStack {
                Text("Capture").frame(width: 80, alignment: .leading).foregroundStyle(.secondary)
                Text(formatRate(appState.captureSampleRate)).font(
                  .system(.body, design: .monospaced))
                Spacer()
              }
              HStack {
                Text("Playback").frame(width: 80, alignment: .leading).foregroundStyle(.secondary)
                Text(formatRate(appState.playbackSampleRate)).font(
                  .system(.body, design: .monospaced))
                Spacer()
              }
              let ratio = Double(appState.playbackSampleRate) / Double(appState.captureSampleRate)
              Text(String(format: "Conversion ratio: %.4f", ratio)).font(.caption).foregroundStyle(
                .tertiary)
            }
          }

          Text(
            "Resamples audio between capture and playback sample rates. Configure sample rates in the Devices page."
          )
          .font(.caption)
          .foregroundStyle(.secondary)
        }
        .disabled(!appState.resamplerEnabled)
        .opacity(appState.resamplerEnabled ? 1.0 : 0.5)

        Spacer()
      }
      .padding()
    }
    .background(Color(nsColor: .controlBackgroundColor))
  }

}
