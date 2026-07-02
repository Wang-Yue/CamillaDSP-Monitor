// StageDetailView - Configuration UI for each pipeline stage

import DSPConfig
import DSPLib
import Observation
import SwiftUI

struct StageDetailView: View {
  let stageIndex: Int
  @Environment(PipelineStore.self) var pipeline

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
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 20) {
        HStack {
          Image(systemName: stage.type.icon)
            .font(.title2)
            .foregroundStyle(stage.isEnabled ? Color.accentColor : .secondary)

          // Allow renaming the stage
          TextField("Stage Name", text: $stage.name)
            .font(.title2.bold())
            .textFieldStyle(.plain)
            .frame(maxWidth: 300)
            .onSubmit { dsp.applyConfig() }

          Spacer()
          Toggle("Enabled", isOn: $stage.isEnabled)
            .onChange(of: stage.isEnabled) { _, _ in dsp.applyConfig() }
        }

        Divider()

        // 1. Channel Selector (Unified for all stages except Matrix Mixer which defines its own mapping)
        if stage.type != .mixer {
          StageChannelSelector(stage: stage)
        }

        // 2. Stage-Specific Options
        Group {
          switch stage.type {
          case .balance: BalanceOptions(stage: stage)
          case .width: WidthOptions(stage: stage)
          case .msProc: MSProcDescription()
          case .phaseInvert: PhaseInvertDescription()
          case .crossfeed: CrossfeedOptions(stage: stage)
          case .eq: EQOptions(stage: stage)
          case .convolution: ConvolutionOptions(stage: stage)
          case .loudness: LoudnessOptions(stage: stage)
          case .emphasis: EmphasisOptions(stage: stage)
          case .dcProtection: DCProtectionDescription()
          case .gain: GainOptions(stage: stage)
          case .delay: DelayOptions(stage: stage)
          case .limiter: LimiterOptions(stage: stage)
          case .mixer: MatrixMixerOptions(stage: stage)
          case .compressor: CompressorOptions(stage: stage)
          case .noiseGate: NoiseGateOptions(stage: stage)
          case .race: RACEOptions(stage: stage)
          case .dither: DitherOptions(stage: stage)
          case .diffEq: DiffEqOptions(stage: stage)
          case .biquadCombo: BiquadComboOptions(stage: stage)
          case .clipper: ClipperOptions(stage: stage)
          case .graphicEQ: GraphicEQOptions(stage: stage)
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

// MARK: - Channel Selector

struct StageChannelSelector: View {
  @Bindable var stage: PipelineStage
  @Environment(AudioDeviceManager.self) var devices
  @Environment(DSPEngineController.self) var dsp
  @Environment(PipelineStore.self) var pipeline

  var body: some View {
    let index = pipeline.stages.firstIndex(where: { $0.id == stage.id }) ?? 0
    let incomingChannels = pipeline.channelCount(
      beforeStageAtIndex: index, captureChannels: devices.captureConfig.channels)

    GroupBox("Target Channels") {
      VStack(alignment: .leading, spacing: 10) {
        if stage.type == .balance || stage.type == .width || stage.type == .msProc
          || stage.type == .crossfeed || stage.type == .race
        {
          // Stereo channel pair picker
          HStack(spacing: 24) {
            VStack(alignment: .leading, spacing: 4) {
              Text("Left Input").font(.caption).foregroundStyle(.secondary)
              Picker("", selection: $stage.leftChannel) {
                ForEach(0..<incomingChannels, id: \.self) { ch in
                  Text("Channel \(ch + 1)").tag(ch)
                }
              }
              .frame(width: 140)
            }

            VStack(alignment: .leading, spacing: 4) {
              Text("Right Input").font(.caption).foregroundStyle(.secondary)
              Picker("", selection: $stage.rightChannel) {
                ForEach(0..<incomingChannels, id: \.self) { ch in
                  Text("Channel \(ch + 1)").tag(ch)
                }
              }
              .frame(width: 140)
            }
            Spacer()
          }
          .onChange(of: stage.leftChannel) { _, _ in dsp.applyConfig() }
          .onChange(of: stage.rightChannel) { _, _ in dsp.applyConfig() }

          Text(
            "This stereo stage will process the selected Left and Right channels. All other channels will pass through unaffected."
          )
          .font(.caption)
          .foregroundStyle(.secondary)
          .padding(.top, 4)
        } else {
          // Multi-channel checkboxes
          if incomingChannels > 0 {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 110))], alignment: .leading, spacing: 8)
            {
              ForEach(0..<incomingChannels, id: \.self) { ch in
                Toggle(
                  "Channel \(ch + 1)",
                  isOn: Binding(
                    get: { stage.channels.contains(ch) },
                    set: { checked in
                      if checked {
                        stage.channels.insert(ch)
                      } else {
                        stage.channels.remove(ch)
                      }
                      dsp.applyConfig()
                    }
                  )
                )
                .toggleStyle(.checkbox)
              }
            }
          } else {
            Text("No audio channels available.")
              .foregroundStyle(.secondary)
          }
        }
      }
      .padding(.vertical, 4)
      .frame(maxWidth: .infinity, alignment: .leading)
    }
  }
}

// MARK: - Balance

struct BalanceOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

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
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

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
      Text("Encodes stereo to Mid (L+R) and Side (L-R) signals at -6.02 dB.")
        .font(.caption)
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.vertical, 4)
    }
  }
}

// MARK: - Phase Invert

struct PhaseInvertDescription: View {
  var body: some View {
    GroupBox("Phase Inversion") {
      Text("Inverts the phase (polarity) of all selected channels.")
        .font(.caption)
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.vertical, 4)
    }
  }
}

// MARK: - Crossfeed

struct CrossfeedOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

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
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp
  @Environment(PipelineStore.self) var pipeline
  @Environment(AudioDeviceManager.self) var devices

  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      if !pipeline.eqPresets.isEmpty {
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
                preset: preset,
                sampleRate: devices.captureConfig.sampleRate)
            }
          }
          .frame(maxWidth: .infinity, alignment: .leading)
          .padding(.vertical, 4)
        }
      } else {
        Text("No EQ presets yet. Create one in the sidebar.")
          .font(.callout)
          .foregroundStyle(.secondary)
          .padding(.horizontal, 4)
      }
    }
  }
}

struct EQSummaryCard: View {
  let preset: EQPreset
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

// MARK: - Convolution

struct ConvolutionOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp
  @Environment(PipelineStore.self) var pipeline
  @Environment(AudioDeviceManager.self) var devices

  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      if pipeline.convPresets.isEmpty {
        Text("No convolution presets yet. Open Room Correction → Generate FIR to create one.")
          .font(.callout)
          .foregroundStyle(.secondary)
          .padding(.horizontal, 4)
      } else {
        GroupBox("Convolution Preset") {
          VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 16) {
              Text("Preset")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .fixedSize()
              ConvPresetPicker(
                selectedID: $stage.convPresetID, presets: pipeline.convPresets
              )
              .frame(maxWidth: 400)
              .onChange(of: stage.convPresetID) { _, _ in dsp.applyConfig() }
              Spacer()
            }

            if let preset = pipeline.convPresets.first(where: { $0.id == stage.convPresetID }) {
              ConvolutionSummaryCard(preset: preset, sampleRate: liveRate)
            }
          }
          .padding(.vertical, 4)
          .frame(maxWidth: .infinity, alignment: .leading)
        }
      }
    }
  }

  private var liveRate: Int { devices.captureConfig.sampleRate }
}

struct ConvolutionSummaryCard: View {
  let preset: ConvolutionPreset
  let sampleRate: Int

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack(spacing: 12) {
        metaCell("Kind", preset.kindLabel)
        metaCell("Taps", "\(preset.taps)")
        metaCell("Rate", rateLabel)
        let ms = preset.latencyMilliseconds(atSampleRate: effectiveRate)
        metaCell("Latency", ms > 0 ? String(format: "%.1f ms", ms) : "≈ 0 ms")
        Spacer()
      }
      .font(.caption)

      if let path = preset.irPath(forSampleRate: sampleRate) {
        ConvolutionIRPlot(irPath: path)
          .frame(height: 110)
          .allowsHitTesting(false)
      } else {
        Text("No IR available for \(sampleRate) Hz.")
          .font(.callout)
          .foregroundStyle(.secondary)
          .padding(.vertical, 8)
      }
    }
    .padding(.vertical, 4)
    .padding(.horizontal, 4)
  }

  private var rateLabel: String {
    let liveRate = sampleRate
    if preset.irPaths[liveRate] != nil {
      return "\(liveRate) Hz"
    }
    let chosen = effectiveRate
    return "\(chosen) Hz · fallback (live = \(liveRate))"
  }

  private var effectiveRate: Int {
    let liveRate = sampleRate
    if preset.irPaths[liveRate] != nil { return liveRate }
    let available = preset.availableSampleRates
    guard !available.isEmpty else { return liveRate }
    let target = log(Double(liveRate))
    return available.min(by: {
      abs(log(Double($0)) - target) < abs(log(Double($1)) - target)
    }) ?? liveRate
  }

  private func metaCell(_ label: String, _ value: String) -> some View {
    VStack(alignment: .leading, spacing: 1) {
      Text(label).foregroundStyle(.secondary)
      Text(value).font(.system(.caption, design: .monospaced))
    }
  }
}

struct ConvPresetPicker: View {
  @Binding var selectedID: UUID?
  let presets: [ConvolutionPreset]
  var body: some View {
    Picker("", selection: $selectedID) {
      Text("None").tag(nil as UUID?)
      ForEach(presets) { preset in
        Text("\(preset.name)  \(preset.kindLabel) · \(preset.taps) taps").tag(preset.id as UUID?)
      }
    }
    .labelsHidden()
  }
}

// MARK: - Loudness

struct LoudnessOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

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
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

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
      Text(
        "First-order highpass at 7 Hz — removes DC offset and subsonic content on all selected channels."
      )
      .font(.caption)
      .foregroundStyle(.secondary)
      .frame(maxWidth: .infinity, alignment: .leading)
      .padding(.vertical, 4)
    }
  }
}

// MARK: - Gain

struct GainOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    GroupBox("Gain / Mute Settings") {
      VStack(alignment: .leading, spacing: 12) {
        HStack(spacing: 16) {
          Text("Gain")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize()

          Slider(value: $stage.gainValue, in: -30...30, step: 0.1)
            .onChange(of: stage.gainValue) { _, _ in dsp.applyConfig() }

          Text(String(format: "%+.1f dB", stage.gainValue))
            .font(.system(.body, design: .monospaced))
            .fixedSize()

          Button("Reset") {
            stage.gainValue = 0.0
            dsp.applyConfig()
          }
          .controlSize(.small)
        }

        Divider()

        HStack(spacing: 24) {
          Toggle("Invert Polarity", isOn: $stage.gainInverted)
            .onChange(of: stage.gainInverted) { _, _ in dsp.applyConfig() }

          Toggle("Mute", isOn: $stage.gainMuted)
            .onChange(of: stage.gainMuted) { _, _ in dsp.applyConfig() }
        }
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - Delay

struct DelayOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    GroupBox("Delay / Time Alignment") {
      VStack(alignment: .leading, spacing: 12) {
        HStack(spacing: 16) {
          Text("Unit")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize()

          Picker("", selection: $stage.delayUnit) {
            Text("Milliseconds (ms)").tag(DelayUnit.ms)
            Text("Microseconds (μs)").tag(DelayUnit.us)
            Text("Samples").tag(DelayUnit.samples)
            Text("Millimeters (mm)").tag(DelayUnit.mm)
          }
          .frame(width: 200)
          .labelsHidden()
          .onChange(of: stage.delayUnit) { _, _ in dsp.applyConfig() }

          Spacer()
        }

        HStack(spacing: 16) {
          Text("Delay")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize()

          let maxVal: Double =
            stage.delayUnit == .samples ? 96000 : (stage.delayUnit == .us ? 1_000_000 : 1000)
          let stepVal: Double = stage.delayUnit == .samples ? 1.0 : 0.1

          Slider(value: $stage.delayValue, in: 0...maxVal, step: stepVal)
            .onChange(of: stage.delayValue) { _, _ in dsp.applyConfig() }

          Text("\(String(format: "%.2f", stage.delayValue)) \(stage.delayUnit.rawValue)")
            .font(.system(.body, design: .monospaced))
            .fixedSize()

          Button("Zero") {
            stage.delayValue = 0.0
            dsp.applyConfig()
          }
          .controlSize(.small)
        }
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - Limiter

struct LimiterOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    GroupBox("Lookahead Peak Limiter") {
      VStack(alignment: .leading, spacing: 12) {
        HStack(spacing: 16) {
          Text("Limit")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize()

          Slider(value: $stage.limiterLimit, in: -30...0, step: 0.1)
            .onChange(of: stage.limiterLimit) { _, _ in dsp.applyConfig() }

          Text(String(format: "%.1f dB", stage.limiterLimit))
            .font(.system(.body, design: .monospaced))
            .fixedSize()
        }

        HStack(spacing: 16) {
          Text("Attack")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize()

          Slider(value: $stage.limiterAttack, in: 0.1...100.0, step: 0.1)
            .onChange(of: stage.limiterAttack) { _, _ in dsp.applyConfig() }

          Text(String(format: "%.1f ms", stage.limiterAttack))
            .font(.system(.body, design: .monospaced))
            .fixedSize()
        }

        HStack(spacing: 16) {
          Text("Release")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize()

          Slider(value: $stage.limiterRelease, in: 5...1000, step: 5)
            .onChange(of: stage.limiterRelease) { _, _ in dsp.applyConfig() }

          Text(String(format: "%.0f ms", stage.limiterRelease))
            .font(.system(.body, design: .monospaced))
            .fixedSize()
        }
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - Matrix Mixer

struct MatrixMixerOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(AudioDeviceManager.self) var devices
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      HStack(spacing: 24) {
        VStack(alignment: .leading, spacing: 4) {
          Text("Input Channels").font(.caption).foregroundStyle(.secondary)
          Picker("", selection: $stage.mixerChannelsIn) {
            ForEach(1...16, id: \.self) { ch in
              Text("\(ch) Channels").tag(ch)
            }
          }
          .frame(width: 140)
          .labelsHidden()
          .onChange(of: stage.mixerChannelsIn) { _, _ in dsp.applyConfig() }
        }

        VStack(alignment: .leading, spacing: 4) {
          Text("Output Channels").font(.caption).foregroundStyle(.secondary)
          Picker("", selection: $stage.mixerChannelsOut) {
            ForEach(1...16, id: \.self) { ch in
              Text("\(ch) Channels").tag(ch)
            }
          }
          .frame(width: 140)
          .labelsHidden()
          .onChange(of: stage.mixerChannelsOut) { _, _ in dsp.applyConfig() }
        }

        Spacer()
      }

      GroupBox("Matrix Mixer Mapping") {
        ScrollView([.horizontal, .vertical]) {
          VStack(alignment: .leading, spacing: 10) {
            // Headers: Inputs
            HStack(spacing: 0) {
              Text("Out \\ In")
                .font(.caption.bold())
                .frame(width: 80, alignment: .leading)

              ForEach(0..<stage.mixerChannelsIn, id: \.self) { inCh in
                Text("Ch \(inCh + 1)")
                  .font(.caption.bold())
                  .frame(width: 90, alignment: .center)
              }
            }

            Divider()

            // Rows: Outputs
            ForEach(0..<stage.mixerChannelsOut, id: \.self) { outCh in
              HStack(spacing: 0) {
                Text("Ch \(outCh + 1)")
                  .font(.body.bold())
                  .frame(width: 80, alignment: .leading)

                ForEach(0..<stage.mixerChannelsIn, id: \.self) { inCh in
                  MatrixCell(stage: stage, dest: outCh, src: inCh)
                    .frame(width: 90)
                }
              }
              Divider()
            }
          }
          .padding(4)
        }

        Button("Reset to 1:1 Passthrough") {
          let minCh = min(stage.mixerChannelsIn, stage.mixerChannelsOut)
          stage.mixerMappings = (0..<stage.mixerChannelsOut).map { i in
            let src = i < minCh ? i : 0
            return MixerMapping(dest: i, sources: [MixerSource(channel: src, gain: 0.0)])
          }
          dsp.applyConfig()
        }
        .controlSize(.small)
        .padding(.top, 8)
      }
    }
  }
}

struct MatrixCell: View {
  @Bindable var stage: PipelineStage
  let dest: Int
  let src: Int
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    let mappingIndex = stage.mixerMappings.firstIndex(where: { $0.dest == dest })
    let sourceIndex: Int? = {
      guard let mappingIndex else { return nil }
      return stage.mixerMappings[mappingIndex].sources.firstIndex(where: { $0.channel == src })
    }()

    let isConnected = sourceIndex != nil

    VStack(spacing: 4) {
      Toggle(
        "",
        isOn: Binding(
          get: { isConnected },
          set: { connected in
            updateConnection(connected: connected)
          }
        )
      )
      .toggleStyle(.checkbox)
      .labelsHidden()

      if isConnected, let mappingIndex, let sourceIndex {
        let source = stage.mixerMappings[mappingIndex].sources[sourceIndex]

        VStack(spacing: 2) {
          TextField(
            "",
            value: Binding(
              get: { source.gain },
              set: { newGain in
                var mappings = stage.mixerMappings
                mappings[mappingIndex].sources[sourceIndex].gain = newGain
                stage.mixerMappings = mappings
                dsp.applyConfig()
              }
            ), format: .number
          )
          .textFieldStyle(.roundedBorder)
          .font(.system(size: 10, design: .monospaced))
          .multilineTextAlignment(.center)
          .frame(width: 60)

          Button(action: {
            var mappings = stage.mixerMappings
            let inv = mappings[mappingIndex].sources[sourceIndex].inverted ?? false
            mappings[mappingIndex].sources[sourceIndex].inverted = !inv
            stage.mixerMappings = mappings
            dsp.applyConfig()
          }) {
            Text("Ø")
              .font(.system(size: 10, weight: .bold))
              .foregroundStyle((source.inverted ?? false) ? Color.orange : Color.secondary)
          }
          .buttonStyle(.plain)
        }
      }
    }
    .frame(height: 70)
    .background(isConnected ? Color.accentColor.opacity(0.05) : Color.clear)
    .contentShape(Rectangle())
  }

  private func updateConnection(connected: Bool) {
    var mappings = stage.mixerMappings

    if !mappings.contains(where: { $0.dest == dest }) {
      mappings.append(MixerMapping(dest: dest, sources: []))
    }

    guard let mappingIndex = mappings.firstIndex(where: { $0.dest == dest }) else { return }

    if connected {
      if !mappings[mappingIndex].sources.contains(where: { $0.channel == src }) {
        mappings[mappingIndex].sources.append(MixerSource(channel: src, gain: 0.0))
      }
    } else {
      mappings[mappingIndex].sources.removeAll { $0.channel == src }
    }

    stage.mixerMappings = mappings
    dsp.applyConfig()
  }
}

// MARK: - Compressor

struct CompressorOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    GroupBox("Dynamics Compressor") {
      VStack(alignment: .leading, spacing: 12) {
        HStack(spacing: 16) {
          Text("Threshold")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.compressorThreshold, in: -60...0, step: 0.5)
            .onChange(of: stage.compressorThreshold) { _, _ in dsp.applyConfig() }
          Text(String(format: "%.1f dB", stage.compressorThreshold))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }

        HStack(spacing: 16) {
          Text("Ratio")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.compressorRatio, in: 1.0...20.0, step: 0.1)
            .onChange(of: stage.compressorRatio) { _, _ in dsp.applyConfig() }
          Text(String(format: "%.1f:1", stage.compressorRatio))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }

        HStack(spacing: 16) {
          Text("Attack")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.compressorAttack, in: 0.1...100.0, step: 0.1)
            .onChange(of: stage.compressorAttack) { _, _ in dsp.applyConfig() }
          Text(String(format: "%.1f ms", stage.compressorAttack))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }

        HStack(spacing: 16) {
          Text("Release")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.compressorRelease, in: 5...1000, step: 5)
            .onChange(of: stage.compressorRelease) { _, _ in dsp.applyConfig() }
          Text(String(format: "%.0f ms", stage.compressorRelease))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }

        HStack(spacing: 16) {
          Text("Makeup Gain")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.compressorMakeupGain, in: 0...30, step: 0.5)
            .onChange(of: stage.compressorMakeupGain) { _, _ in dsp.applyConfig() }
          Text(String(format: "%+.1f dB", stage.compressorMakeupGain))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }

        Divider()

        VStack(alignment: .leading, spacing: 8) {
          Toggle("Enable Soft Clip", isOn: $stage.compressorSoftClip)
            .onChange(of: stage.compressorSoftClip) { _, _ in dsp.applyConfig() }

          if stage.compressorSoftClip {
            HStack(spacing: 16) {
              Text("Clip Limit")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .frame(width: 90, alignment: .leading)
              Slider(value: $stage.compressorClipLimit, in: -10...0, step: 0.1)
                .onChange(of: stage.compressorClipLimit) { _, _ in dsp.applyConfig() }
              Text(String(format: "%.1f dB", stage.compressorClipLimit))
                .font(.system(.body, design: .monospaced))
                .frame(width: 70, alignment: .trailing)
            }
            .transition(.opacity)
          }
        }
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - Noise Gate

struct NoiseGateOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    GroupBox("Noise Gate") {
      VStack(alignment: .leading, spacing: 12) {
        HStack(spacing: 16) {
          Text("Threshold")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.gateThreshold, in: -100...0, step: 0.5)
            .onChange(of: stage.gateThreshold) { _, _ in dsp.applyConfig() }
          Text(String(format: "%.1f dB", stage.gateThreshold))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }

        HStack(spacing: 16) {
          Text("Attenuation")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.gateAttenuation, in: -100...0, step: 0.5)
            .onChange(of: stage.gateAttenuation) { _, _ in dsp.applyConfig() }
          Text(String(format: "%.1f dB", stage.gateAttenuation))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }

        HStack(spacing: 16) {
          Text("Attack")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.gateAttack, in: 0.1...100.0, step: 0.1)
            .onChange(of: stage.gateAttack) { _, _ in dsp.applyConfig() }
          Text(String(format: "%.1f ms", stage.gateAttack))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }

        HStack(spacing: 16) {
          Text("Release")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.gateRelease, in: 5...1000, step: 5)
            .onChange(of: stage.gateRelease) { _, _ in dsp.applyConfig() }
          Text(String(format: "%.0f ms", stage.gateRelease))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - RACE

struct RACEOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    GroupBox("RACE Crosstalk Cancellation") {
      VStack(alignment: .leading, spacing: 12) {
        Text(
          "Receiver Active Crosstalk Cancellation (RACE) implements a 3D audio effect for speaker playback by canceling acoustic crosstalk between two channels."
        )
        .font(.caption)
        .foregroundStyle(.secondary)
        .padding(.bottom, 4)

        HStack(spacing: 16) {
          Text("Delay")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.raceDelay, in: 0.01...2.0, step: 0.01)
            .onChange(of: stage.raceDelay) { _, _ in dsp.applyConfig() }
          Text(String(format: "%.2f ms", stage.raceDelay))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }

        HStack(spacing: 16) {
          Text("Attenuation")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.raceAttenuation, in: 1.0...20.0, step: 0.1)
            .onChange(of: stage.raceAttenuation) { _, _ in dsp.applyConfig() }
          Text(String(format: "%.1f dB", stage.raceAttenuation))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - Dither

struct DitherOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    GroupBox("Dither Noise Shaping") {
      VStack(alignment: .leading, spacing: 12) {
        HStack(spacing: 16) {
          Text("Type")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Picker("", selection: $stage.ditherType) {
            Text("None").tag(DitherType.none)
            Text("Flat").tag(DitherType.flat)
            Text("Highpass").tag(DitherType.highpass)
            Group {
              Text("F-weighted 44.1k").tag(DitherType.fweighted441)
              Text("F-weighted Long 44.1k").tag(DitherType.fweightedLong441)
              Text("F-weighted Short 44.1k").tag(DitherType.fweightedShort441)
              Text("Gesemann 44.1k").tag(DitherType.gesemann441)
              Text("Gesemann 48k").tag(DitherType.gesemann48)
            }
            Group {
              Text("Lipshitz 44.1k").tag(DitherType.lipshitz441)
              Text("Lipshitz Long 44.1k").tag(DitherType.lipshitzLong441)
              Text("Shibata 44.1k").tag(DitherType.shibata441)
              Text("Shibata High 44.1k").tag(DitherType.shibataHigh441)
              Text("Shibata Low 44.1k").tag(DitherType.shibataLow441)
            }
            Group {
              Text("Shibata 48k").tag(DitherType.shibata48)
              Text("Shibata High 48k").tag(DitherType.shibataHigh48)
              Text("Shibata Low 48k").tag(DitherType.shibataLow48)
              Text("Shibata 96k").tag(DitherType.shibata96)
              Text("Shibata Low 96k").tag(DitherType.shibataLow96)
            }
          }
          .frame(width: 240)
          .labelsHidden()
          .onChange(of: stage.ditherType) { _, _ in dsp.applyConfig() }
          Spacer()
        }

        HStack(spacing: 16) {
          Text("Bit Depth")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Picker("", selection: $stage.ditherBits) {
            Text("16-bit").tag(16)
            Text("24-bit").tag(24)
            Text("32-bit").tag(32)
            Text("8-bit (Lofi)").tag(8)
          }
          .frame(width: 120)
          .labelsHidden()
          .onChange(of: stage.ditherBits) { _, _ in dsp.applyConfig() }
          Spacer()
        }

        HStack(spacing: 16) {
          Text("Amplitude")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.ditherAmplitude, in: 0.0...10.0, step: 0.1)
            .onChange(of: stage.ditherAmplitude) { _, _ in dsp.applyConfig() }
          Text(String(format: "%.1f", stage.ditherAmplitude))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - DiffEq

struct DiffEqOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    GroupBox("Differential Equation Filter") {
      VStack(alignment: .leading, spacing: 12) {
        Text(
          "Direct form II IIR filter coefficients. Specify as comma-separated lists of decimal numbers."
        )
        .font(.caption)
        .foregroundStyle(.secondary)
        .padding(.bottom, 4)

        VStack(alignment: .leading, spacing: 4) {
          Text("Feedforward Coefficients (b)").font(.caption).foregroundStyle(.secondary)
          TextField("e.g. 1.0, 0.5, 0.25", text: $stage.diffEqB)
            .textFieldStyle(.roundedBorder)
            .onSubmit { dsp.applyConfig() }
        }

        VStack(alignment: .leading, spacing: 4) {
          Text("Feedback Coefficients (a)").font(.caption).foregroundStyle(.secondary)
          TextField("e.g. 1.0, -0.5, 0.1", text: $stage.diffEqA)
            .textFieldStyle(.roundedBorder)
            .onSubmit { dsp.applyConfig() }
        }
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - Biquad Combo

struct BiquadComboOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    GroupBox("Biquad Combo / Crossovers") {
      VStack(alignment: .leading, spacing: 12) {
        HStack(spacing: 16) {
          Text("Combo Type")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Picker("", selection: $stage.comboType) {
            Text("Butterworth Lowpass").tag(BiquadComboType.butterworthLowpass)
            Text("Butterworth Highpass").tag(BiquadComboType.butterworthHighpass)
            Text("Linkwitz-Riley Lowpass").tag(BiquadComboType.linkwitzRileyLowpass)
            Text("Linkwitz-Riley Highpass").tag(BiquadComboType.linkwitzRileyHighpass)
            Text("Tilt").tag(BiquadComboType.tilt)
          }
          .frame(width: 220)
          .labelsHidden()
          .onChange(of: stage.comboType) { _, _ in dsp.applyConfig() }
          Spacer()
        }

        HStack(spacing: 16) {
          Text("Frequency")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.comboFreq, in: 20...20000, step: 1)
            .onChange(of: stage.comboFreq) { _, _ in dsp.applyConfig() }
          Text("\(Int(stage.comboFreq)) Hz")
            .font(.system(.body, design: .monospaced))
            .frame(width: 75, alignment: .trailing)
        }

        if stage.comboType == .butterworthLowpass || stage.comboType == .butterworthHighpass
          || stage.comboType == .linkwitzRileyLowpass || stage.comboType == .linkwitzRileyHighpass
        {
          HStack(spacing: 16) {
            Text("Filter Order")
              .font(.subheadline)
              .foregroundStyle(.secondary)
              .frame(width: 90, alignment: .leading)
            Picker("", selection: $stage.comboOrder) {
              Text("2nd Order (12 dB/oct)").tag(2)
              Text("4th Order (24 dB/oct)").tag(4)
              Text("6th Order (36 dB/oct)").tag(6)
              Text("8th Order (48 dB/oct)").tag(8)
            }
            .frame(width: 200)
            .labelsHidden()
            .onChange(of: stage.comboOrder) { _, _ in dsp.applyConfig() }
            Spacer()
          }
        }

        if stage.comboType == .tilt {
          HStack(spacing: 16) {
            Text("Gain")
              .font(.subheadline)
              .foregroundStyle(.secondary)
              .frame(width: 90, alignment: .leading)
            Slider(value: $stage.comboGain, in: -15...15, step: 0.1)
              .onChange(of: stage.comboGain) { _, _ in dsp.applyConfig() }
            Text(String(format: "%+.1f dB", stage.comboGain))
              .font(.system(.body, design: .monospaced))
              .frame(width: 75, alignment: .trailing)
          }
        }
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - Clipper

struct ClipperOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    GroupBox("Hard / Soft Clipper") {
      VStack(alignment: .leading, spacing: 12) {
        HStack(spacing: 16) {
          Text("Clip Limit")
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(width: 90, alignment: .leading)
          Slider(value: $stage.clipperLimit, in: -30...0, step: 0.1)
            .onChange(of: stage.clipperLimit) { _, _ in dsp.applyConfig() }
          Text(String(format: "%.1f dB", stage.clipperLimit))
            .font(.system(.body, design: .monospaced))
            .frame(width: 70, alignment: .trailing)
        }

        Toggle("Enable Soft Clipping", isOn: $stage.clipperSoftClip)
          .onChange(of: stage.clipperSoftClip) { _, _ in dsp.applyConfig() }
      }
      .padding(.vertical, 4)
    }
  }
}

// MARK: - Graphic EQ

struct GraphicEQOptions: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    GroupBox("Graphic Equalizer Settings") {
      VStack(alignment: .leading, spacing: 16) {
        HStack(spacing: 24) {
          VStack(alignment: .leading, spacing: 4) {
            Text("Frequency Range").font(.caption).foregroundStyle(.secondary)
            HStack(spacing: 8) {
              TextField("Min", value: $stage.graphicEQFreqMin, format: .number)
                .textFieldStyle(.roundedBorder)
                .frame(width: 70)
              Text("to")
              TextField("Max", value: $stage.graphicEQFreqMax, format: .number)
                .textFieldStyle(.roundedBorder)
                .frame(width: 80)
              Text("Hz")
            }
            .onSubmit { dsp.applyConfig() }
          }

          VStack(alignment: .leading, spacing: 4) {
            Text("Bands").font(.caption).foregroundStyle(.secondary)
            Stepper(
              "\(stage.graphicEQBandCount) Bands",
              value: Binding(
                get: { stage.graphicEQBandCount },
                set: { newVal in
                  stage.graphicEQBandCount = newVal
                  dsp.applyConfig()
                }
              ), in: 2...64
            )
            .controlSize(.small)
          }

          Spacer()
        }

        Divider()

        ScrollView(.horizontal, showsIndicators: true) {
          HStack(spacing: 14) {
            ForEach(0..<stage.graphicEQBandCount, id: \.self) { index in
              let freq = bandFrequency(
                index: index, total: stage.graphicEQBandCount, fMin: stage.graphicEQFreqMin,
                fMax: stage.graphicEQFreqMax)

              VStack(spacing: 8) {
                Text(
                  String(
                    format: "%.1f",
                    index < stage.graphicEQGains.count ? stage.graphicEQGains[index] : 0.0)
                )
                .font(.system(size: 9, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(width: 35)

                VSlider(
                  value: Binding(
                    get: {
                      guard index < stage.graphicEQGains.count else { return 0.0 }
                      return stage.graphicEQGains[index]
                    },
                    set: { newVal in
                      guard index < stage.graphicEQGains.count else { return }
                      stage.graphicEQGains[index] = newVal
                      dsp.applyConfig()
                    }
                  ), in: -12...12
                )
                .frame(height: 160)

                Text(freqLabel(freq))
                  .font(.system(size: 9, weight: .bold))
                  .rotationEffect(.degrees(-90))
                  .frame(height: 30)
                  .frame(width: 35)
              }
            }
          }
          .padding(.vertical, 8)
          .padding(.horizontal, 4)
        }

        HStack {
          Button("Reset All to 0 dB") {
            stage.graphicEQGains = Array(repeating: 0.0, count: stage.graphicEQBandCount)
            dsp.applyConfig()
          }
          .controlSize(.small)

          Spacer()
        }
      }
      .padding(.vertical, 4)
    }
  }

  private func bandFrequency(index: Int, total: Int, fMin: Double, fMax: Double) -> Double {
    guard total > 1 else { return fMin }
    let ratio = fMax / fMin
    let exponent = Double(index) / Double(total - 1)
    return fMin * pow(ratio, exponent)
  }

  private func freqLabel(_ hz: Double) -> String {
    if hz >= 1000 {
      let khz = hz / 1000.0
      if khz == Double(Int(khz)) {
        return "\(Int(khz))k"
      } else {
        return String(format: "%.1fk", khz)
      }
    } else {
      if hz == Double(Int(hz)) {
        return "\(Int(hz))"
      } else {
        return String(format: "%.1f", hz)
      }
    }
  }
}

// MARK: - Custom Vertical Slider

struct VSlider: View {
  @Binding var value: Double
  let range: ClosedRange<Double>

  init(value: Binding<Double>, in range: ClosedRange<Double>) {
    self._value = value
    self.range = range
  }

  var body: some View {
    GeometryReader { geometry in
      let height = geometry.size.height
      let width = geometry.size.width
      let trackWidth: CGFloat = 4
      let knobSize: CGFloat = 14

      let pct = CGFloat((value - range.lowerBound) / (range.upperBound - range.lowerBound))
      let knobY = height - (pct * height)

      ZStack {
        // Track
        RoundedRectangle(cornerRadius: trackWidth / 2)
          .fill(Color.secondary.opacity(0.2))
          .frame(width: trackWidth)
          .frame(maxHeight: .infinity)

        // Active track growing from 0 dB center
        let centerPct = CGFloat((0.0 - range.lowerBound) / (range.upperBound - range.lowerBound))
        let centerY = height - (centerPct * height)
        let activeHeight = abs(knobY - centerY)
        let activeY = min(knobY, centerY)

        RoundedRectangle(cornerRadius: trackWidth / 2)
          .fill(Color.accentColor)
          .frame(width: trackWidth, height: activeHeight)
          .position(x: width / 2, y: activeY + activeHeight / 2)

        // Center tick line (0 dB)
        Rectangle()
          .fill(Color.secondary.opacity(0.5))
          .frame(width: 12, height: 1)
          .position(x: width / 2, y: centerY)

        // Knob
        Circle()
          .fill(Color.white)
          .frame(width: knobSize, height: knobSize)
          .shadow(radius: 2)
          .overlay(
            Circle()
              .stroke(Color.accentColor, lineWidth: 1.5)
          )
          .position(x: width / 2, y: knobY)
      }
      .contentShape(Rectangle())
      .gesture(
        DragGesture(minimumDistance: 0)
          .onChanged { gesture in
            let y = gesture.location.y
            let clampedY = max(0, min(height, y))
            let newPct = Double((height - clampedY) / height)
            let newVal = range.lowerBound + newPct * (range.upperBound - range.lowerBound)
            value = newVal
          }
      )
    }
    .frame(width: 20)
  }
}
