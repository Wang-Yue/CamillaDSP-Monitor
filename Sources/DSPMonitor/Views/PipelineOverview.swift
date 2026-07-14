// PipelineOverview.swift - Pipeline overview components for Dashboard and MiniPlayer

import AppKit
import DSPConfig
import Observation
import SwiftUI

// MARK: - Mini Pipeline Overview (For Mini Player)

struct MiniPipelineOverview: View {
  @Environment(DSPEngineController.self) var dsp
  @Environment(AudioDeviceManager.self) var devices
  @Environment(AudioSettings.self) var settings
  @Environment(PipelineStore.self) var pipeline

  var body: some View {
    HorizontalScrollWithVerticalWheel {
      HStack(spacing: 4) {
        StageChip(
          icon: "mic", label: devices.captureConfig.deviceName ?? "Input", color: .blue,
          isActive: dsp.status == .running, compact: true)
        Image(systemName: "chevron.right").foregroundStyle(.tertiary).font(.caption)
        Button {
          settings.resamplerEnabled.toggle()
        } label: {
          StageChip(
            icon: "arrow.triangle.2.circlepath", label: "Resampler",
            color: settings.resamplerEnabled ? Color.accentColor : .gray,
            isActive: settings.resamplerEnabled, compact: true)
        }.buttonStyle(.plain)
        Image(systemName: "chevron.right").foregroundStyle(.tertiary).font(.caption)
        ForEach(pipeline.stages) { stage in
          StageChipButton(stage: stage, compact: true)
          Image(systemName: "chevron.right").foregroundStyle(.tertiary).font(.caption)
        }
        StageChip(
          icon: "hifispeaker", label: devices.playbackConfig.deviceName ?? "Output",
          color: .green, isActive: dsp.status == .running, compact: true)
      }.padding(.vertical, 4)
    }
  }
}

// MARK: - Stage Chip Components

struct StageChip: View {
  let icon: String
  let label: String
  let color: Color
  let isActive: Bool
  var compact: Bool = false

  var body: some View {
    HStack(spacing: compact ? 3 : 6) {
      Image(systemName: icon)
        .font(compact ? .system(size: 8) : .caption)
      Text(label)
        .font(compact ? .system(size: 9, weight: isActive ? .semibold : .regular) : .caption)
        .lineLimit(1)
    }
    .padding(.horizontal, compact ? 6 : 10)
    .padding(.vertical, compact ? 4 : 6)
    .background(
      compact
        ? (isActive ? color : Color.gray.opacity(0.3))
        : (isActive ? color.opacity(0.15) : Color.gray.opacity(0.08))
    )
    .foregroundStyle(
      compact
        ? (isActive ? AnyShapeStyle(.black) : AnyShapeStyle(.white.opacity(0.6)))
        : (isActive ? AnyShapeStyle(color) : AnyShapeStyle(.secondary))
    )
    .clipShape(Capsule())
    .modifier(StageChipBorderModifier(color: color, isActive: isActive, compact: compact))
  }
}

private struct StageChipBorderModifier: ViewModifier {
  let color: Color
  let isActive: Bool
  let compact: Bool
  func body(content: Content) -> some View {
    if compact {
      content
    } else {
      content
        .onHover { h in if h { NSCursor.pointingHand.push() } else { NSCursor.pop() } }
        .overlay(Capsule().stroke(isActive ? color.opacity(0.3) : Color.clear, lineWidth: 1))
    }
  }
}

struct StageChipButton: View {
  let stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp
  var compact: Bool = false

  var body: some View {
    Button {
      stage.isEnabled.toggle()
      dsp.applyConfig()
    } label: {
      StageChip(
        icon: stage.type.icon, label: stage.name,
        color: stage.isEnabled ? Color.accentColor : .gray,
        isActive: stage.isEnabled, compact: compact)
    }.buttonStyle(.plain)
  }
}

// MARK: - Pipeline Overview Card (For Dashboard)

struct PipelineOverviewCard: View {
  @Environment(AppState.self) var appState
  @Environment(DSPEngineController.self) var dsp
  @Environment(AudioDeviceManager.self) var devices
  @Environment(AudioSettings.self) var settings
  @Environment(PipelineStore.self) var pipeline

  @State private var showElementaryDetails: Bool = true
  @State private var hoveredStageID: UUID? = nil
  @State private var hoveredChannel: Int? = nil

  private var captureChannels: Int {
    devices.captureConfig.channels
  }

  private var captureSampleRate: Int {
    devices.captureConfig.sampleRate
  }

  private var playbackChannels: Int {
    devices.playbackConfig.channels
  }

  private var playbackSampleRate: Int {
    devices.playbackConfig.sampleRate
  }

  private var sampleRate: Int {
    devices.captureConfig.sampleRate
  }

  private var finalPipelineOutputChannels: Int {
    let stages = pipeline.stages
    if stages.isEmpty {
      return captureChannels
    }
    return pipeline.channelCount(beforeStageAtIndex: stages.count, captureChannels: captureChannels)
  }

  private var isOutputChannelMismatch: Bool {
    finalPipelineOutputChannels != playbackChannels
  }

  private var totalElementarySteps: Int {
    var count = 0
    for (i, stage) in pipeline.stages.enumerated() {
      let inCh = pipeline.channelCount(beforeStageAtIndex: i, captureChannels: captureChannels)
      let steps = stage.buildPipelineSteps(
        channelCount: inCh,
        eqPresets: pipeline.eqPresets,
        convPresets: pipeline.convPresets,
        sampleRate: sampleRate
      )
      count += steps.count
    }
    return count
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      // Card Header & Controls
      headerView

      // Graph Canvas
      HorizontalScrollWithVerticalWheel {
        HStack(alignment: .center, spacing: 0) {
          // 1. Input / Capture Node
          captureNodeView

          // Connector: Capture -> Resampler/Pipeline
          let postCaptureCh = captureChannels
          let resamplerInCh = postCaptureCh
          channelConnector(fromChannels: postCaptureCh, toChannels: resamplerInCh)

          // 2. Resampler Node (if enabled)
          if settings.resamplerEnabled {
            resamplerNodeView
            channelConnector(fromChannels: resamplerInCh, toChannels: resamplerInCh)
          }

          // 3. Pipeline Stages & Elementary Steps
          let stages = pipeline.stages
          if stages.isEmpty {
            channelConnector(fromChannels: resamplerInCh, toChannels: playbackChannels, isMismatched: resamplerInCh != playbackChannels)
          } else {
            ForEach(Array(stages.enumerated()), id: \.element.id) { index, stage in
              let inCh = pipeline.channelCount(beforeStageAtIndex: index, captureChannels: captureChannels)
              let outCh = stage.isActive && stage.type == .mixer ? stage.mixerChannelsOut : inCh
              let nextInCh = (index + 1 < stages.count)
                ? pipeline.channelCount(beforeStageAtIndex: index + 1, captureChannels: captureChannels)
                : playbackChannels
              let isMismatched = outCh != nextInCh

              HStack(alignment: .center, spacing: 0) {
                stageGraphNode(
                  stage: stage,
                  index: index,
                  incomingChannels: inCh,
                  outgoingChannels: outCh
                )

                channelConnector(fromChannels: outCh, toChannels: nextInCh, isMismatched: isMismatched)
              }
            }
          }

          // 4. Output / Playback Node
          playbackNodeView
        }
        .padding(.vertical, 8)
        .padding(.horizontal, 4)
      }
    }
    .padding()
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
  }

  // MARK: - Header View

  @ViewBuilder
  private var headerView: some View {
    HStack(alignment: .center) {
      VStack(alignment: .leading, spacing: 3) {
        HStack(spacing: 8) {
          Image(systemName: isOutputChannelMismatch ? "exclamationmark.triangle.fill" : "chart.projective")
            .font(.title3)
            .foregroundStyle(isOutputChannelMismatch ? Color.red : Color.accentColor)
          Text("Signal Chain")
            .font(.headline)

          if isOutputChannelMismatch {
            HStack(spacing: 4) {
              Image(systemName: "xmark.octagon.fill")
                .font(.system(size: 10, weight: .bold))
              Text("Broken Chain: Outputs \(finalPipelineOutputChannels) ch, Device expects \(playbackChannels) ch")
                .font(.system(size: 10, weight: .bold))
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .background(Color.red.opacity(0.15))
            .foregroundStyle(Color.red)
            .cornerRadius(6)
            .overlay(
              RoundedRectangle(cornerRadius: 6)
                .stroke(Color.red.opacity(0.4), lineWidth: 1)
            )
          }
        }

        HStack(spacing: 12) {
          Label("\(captureChannels) Ch In (\(formatSampleRate(captureSampleRate)))", systemImage: "arrow.right.to.line.compact")
          Text("•")
          Label("\(pipeline.stages.filter(\.isEnabled).count) Active Stages", systemImage: "square.stack.3d.up.fill")
          Text("•")
          Label("\(totalElementarySteps) Elementary Steps", systemImage: "cpu")
          Text("•")
          Label("\(playbackChannels) Ch Out (\(formatSampleRate(playbackSampleRate)))", systemImage: "arrow.right.line.compact")
        }
        .font(.caption)
        .foregroundStyle(.secondary)
      }

      Spacer()

      // Toggle for Elementary Sub-Steps Details
      Button {
        withAnimation(.easeInOut(duration: 0.2)) {
          showElementaryDetails.toggle()
        }
      } label: {
        Label(
          showElementaryDetails ? "Hide Elementary Steps" : "Show Elementary Steps",
          systemImage: showElementaryDetails ? "list.bullet.indent" : "rectangle.grid.1x2"
        )
        .font(.caption)
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(showElementaryDetails ? Color.accentColor.opacity(0.15) : Color.gray.opacity(0.15))
        .foregroundStyle(showElementaryDetails ? Color.accentColor : Color.primary)
        .clipShape(Capsule())
      }
      .buttonStyle(.plain)
    }
  }

  // MARK: - Capture / Input Node

  @ViewBuilder
  private var captureNodeView: some View {
    GraphNodeCard(
      title: "Input (Capture)",
      subtitle: devices.captureConfig.deviceName ?? "CoreAudio Input",
      icon: "mic.fill",
      color: .blue,
      isActive: dsp.status == .running
    ) {
      VStack(alignment: .leading, spacing: 4) {
        HStack(spacing: 4) {
          Text("Channels: \(captureChannels)")
          Text("•")
          Text(formatSampleRate(captureSampleRate))
        }
        .font(.caption2)
        .foregroundStyle(.secondary)

        HStack(spacing: 4) {
          ForEach(Array(0..<captureChannels), id: \.self) { ch in
            channelBadge(ch: ch)
          }
        }
      }
    }
  }

  // MARK: - Resampler Node

  @ViewBuilder
  private var resamplerNodeView: some View {
    GraphNodeCard(
      title: "Resampler",
      subtitle: "Synchronous SRC",
      icon: "arrow.triangle.2.circlepath",
      color: Color.accentColor,
      isActive: settings.resamplerEnabled
    ) {
      VStack(alignment: .leading, spacing: 4) {
        Text("All \(captureChannels) channels")
          .font(.caption2)
          .foregroundStyle(.secondary)

        Text("\(sampleRate) Hz target")
          .font(.system(size: 10, weight: .medium, design: .monospaced))
          .foregroundStyle(.tertiary)
      }
    }
  }

  // MARK: - Stage Graph Node

  @ViewBuilder
  private func stageGraphNode(
    stage: PipelineStage,
    index: Int,
    incomingChannels: Int,
    outgoingChannels: Int
  ) -> some View {
    let active = stage.isEnabled && stage.isActive
    let categoryColor = categoryColor(for: stage.type)
    let elementarySteps = stage.buildPipelineSteps(
      channelCount: incomingChannels,
      eqPresets: pipeline.eqPresets,
      convPresets: pipeline.convPresets,
      sampleRate: sampleRate
    )

    GraphNodeCard(
      title: stage.name,
      subtitle: stage.type.rawValue,
      icon: stage.type.icon,
      color: active ? categoryColor : .gray,
      isActive: active,
      isHovered: hoveredStageID == stage.id,
      onToggleActive: {
        stage.isEnabled.toggle()
        dsp.applyConfig()
      }
    ) {
      VStack(alignment: .leading, spacing: 6) {
        // Channel overview badge
        HStack(spacing: 6) {
          Label(
            "\(incomingChannels) In \(outgoingChannels != incomingChannels ? "➔ \(outgoingChannels) Out" : "")",
            systemImage: "waveform.path"
          )
          .font(.system(size: 10, weight: .medium))
          .padding(.horizontal, 6)
          .padding(.vertical, 2)
          .background(Color.primary.opacity(0.06))
          .cornerRadius(4)
        }

        // Expanded Elementary Steps
        if showElementaryDetails && active {
          VStack(alignment: .leading, spacing: 6) {
            Divider().opacity(0.3)

            if elementarySteps.isEmpty {
              Text("Passthrough (No elementary steps)")
                .font(.caption2)
                .foregroundStyle(.tertiary)
            } else {
              ForEach(Array(elementarySteps.enumerated()), id: \.offset) { stepIdx, step in
                elementaryStepBadge(step: step, stage: stage, incomingChannels: incomingChannels)
              }
            }
          }
        }
      }
    }
    .onHover { h in
      hoveredStageID = h ? stage.id : nil
    }
  }

  // MARK: - Elementary Step Component Badge

  @ViewBuilder
  private func elementaryStepBadge(
    step: PipelineStep,
    stage: PipelineStage,
    incomingChannels: Int
  ) -> some View {
    VStack(alignment: .leading, spacing: 3) {
      HStack(spacing: 5) {
        stepTypeIcon(step.type)
          .font(.system(size: 9))
          .foregroundStyle(stepTypeColor(step.type))

        Text(stepTypeTitle(step.type))
          .font(.system(size: 10, weight: .semibold))
          .foregroundStyle(.primary)

        Spacer(minLength: 4)

        // Target channels tag
        targetChannelsTag(step: step, stage: stage, incomingChannels: incomingChannels)
      }

      // Step Filter / Mixer details
      if let names = step.names, !names.isEmpty {
        VStack(alignment: .leading, spacing: 1) {
          ForEach(names, id: \.self) { rawName in
            HStack(spacing: 3) {
              Circle()
                .fill(stepTypeColor(step.type).opacity(0.8))
                .frame(width: 3, height: 3)
              Text(readableFilterName(rawName, stage: stage))
                .font(.system(size: 9, design: .monospaced))
                .foregroundStyle(.secondary)
                .lineLimit(1)
            }
          }
        }
        .padding(.leading, 12)
      } else if let name = step.name {
        Text(readableMixerOrProcessorName(name, stage: stage))
          .font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.secondary)
          .padding(.leading, 12)
      }
    }
    .padding(6)
    .background(stepTypeColor(step.type).opacity(0.08))
    .cornerRadius(6)
    .overlay(
      RoundedRectangle(cornerRadius: 6)
        .stroke(stepTypeColor(step.type).opacity(0.2), lineWidth: 0.5)
    )
  }

  // MARK: - Target Channels Tag

  @ViewBuilder
  private func targetChannelsTag(step: PipelineStep, stage: PipelineStage, incomingChannels: Int) -> some View {
    let chList: [Int] = {
      if let chs = step.channels {
        return chs
      } else if let ch = step.channel {
        return [ch]
      } else if step.type == .processor {
        return stage.channels.sorted()
      } else {
        return Array(0..<incomingChannels)
      }
    }()

    HStack(spacing: 2) {
      ForEach(chList, id: \.self) { ch in
        Text(channelLabel(ch))
          .font(.system(size: 8, weight: .bold))
          .padding(.horizontal, 3)
          .padding(.vertical, 1)
          .background(channelColor(ch).opacity(0.2))
          .foregroundStyle(channelColor(ch))
          .cornerRadius(3)
      }
    }
  }

  // MARK: - Output / Playback Node

  @ViewBuilder
  private var playbackNodeView: some View {
    GraphNodeCard(
      title: "Output (Playback)",
      subtitle: devices.playbackConfig.deviceName ?? "CoreAudio Output",
      icon: isOutputChannelMismatch ? "exclamationmark.triangle.fill" : "hifispeaker.fill",
      color: isOutputChannelMismatch ? .red : .green,
      isActive: dsp.status == .running,
      isWarning: isOutputChannelMismatch
    ) {
      VStack(alignment: .leading, spacing: 4) {
        if isOutputChannelMismatch {
          HStack(spacing: 3) {
            Image(systemName: "xmark.octagon.fill")
              .font(.system(size: 9))
              .foregroundStyle(.red)
            Text("Got \(finalPipelineOutputChannels) ch, Device expects \(playbackChannels) ch")
              .font(.system(size: 8, weight: .bold))
              .foregroundStyle(.red)
              .lineLimit(1)
          }
          .padding(.vertical, 1)
        }

        HStack(spacing: 4) {
          Text("Channels: \(playbackChannels)")
          Text("•")
          Text(formatSampleRate(playbackSampleRate))
        }
        .font(.caption2)
        .foregroundStyle(.secondary)

        HStack(spacing: 4) {
          ForEach(Array(0..<playbackChannels), id: \.self) { ch in
            channelBadge(ch: ch)
          }
        }
      }
    }
  }

  private func formatSampleRate(_ rate: Int) -> String {
    if rate % 1000 == 0 {
      return "\(rate / 1000) kHz"
    } else {
      let kHz = Double(rate) / 1000.0
      return String(format: "%.1f kHz", kHz)
    }
  }

  // MARK: - Channel Flow Connector Lines

  @ViewBuilder
  private func channelConnector(fromChannels: Int, toChannels: Int, isMismatched: Bool = false) -> some View {
    VStack(spacing: 4) {
      HStack(spacing: 2) {
        Rectangle()
          .fill(
            LinearGradient(
              colors: isMismatched
                ? [Color.red.opacity(0.6), Color.red]
                : [Color.accentColor.opacity(0.4), Color.accentColor.opacity(0.8)],
              startPoint: .leading,
              endPoint: .trailing
            )
          )
          .frame(width: 24, height: 2)

        Image(systemName: isMismatched ? "exclamationmark.triangle.fill" : "chevron.right")
          .font(.system(size: isMismatched ? 9 : 8, weight: .bold))
          .foregroundStyle(isMismatched ? Color.red : Color.accentColor.opacity(0.8))
      }

      Text(isMismatched ? "\(fromChannels) ch ❌" : "\(fromChannels)\(fromChannels != toChannels ? "➔\(toChannels)" : "") ch")
        .font(.system(size: 8, weight: .bold, design: .monospaced))
        .padding(.horizontal, 4)
        .padding(.vertical, 1)
        .background(isMismatched ? Color.red.opacity(0.15) : Color.primary.opacity(0.05))
        .foregroundStyle(isMismatched ? Color.red : .secondary)
        .cornerRadius(3)
    }
    .frame(width: 52)
  }

  // MARK: - Helper Functions & Color Resolvers

  @ViewBuilder
  private func channelBadge(ch: Int) -> some View {
    Text(channelLabel(ch))
      .font(.system(size: 9, weight: .bold))
      .padding(.horizontal, 5)
      .padding(.vertical, 2)
      .background(channelColor(ch).opacity(0.18))
      .foregroundStyle(channelColor(ch))
      .clipShape(Capsule())
  }

  private func channelLabel(_ index: Int) -> String {
    "\(index + 1)"
  }

  private func channelColor(_ index: Int) -> Color {
    switch index {
    case 0: return .blue
    case 1: return .purple
    case 2: return .orange
    case 3: return .green
    case 4: return .cyan
    default: return .pink
    }
  }

  private func categoryColor(for type: StageType) -> Color {
    switch type.category {
    case .filters: return .accentColor
    case .mixer: return .purple
    case .processors: return .orange
    case .others: return .teal
    }
  }

  private func stepTypeIcon(_ type: PipelineStepType) -> Image {
    switch type {
    case .filter: return Image(systemName: "slider.horizontal.3")
    case .mixer: return Image(systemName: "grid")
    case .processor: return Image(systemName: "cpu")
    }
  }

  private func stepTypeColor(_ type: PipelineStepType) -> Color {
    switch type {
    case .filter: return .blue
    case .mixer: return .purple
    case .processor: return .orange
    }
  }

  private func stepTypeTitle(_ type: PipelineStepType) -> String {
    switch type {
    case .filter: return "Filter Chain"
    case .mixer: return "Matrix / Routing Mixer"
    case .processor: return "Dynamics Processor"
    }
  }

  private func readableFilterName(_ rawName: String, stage: PipelineStage) -> String {
    let parts = rawName.components(separatedBy: "_")
    guard let suffix = parts.last else { return rawName }

    switch suffix {
    case "preamp":
      let gain = pipeline.eqPresets.first(where: { $0.id == stage.eqPresetID })?.preampGain ?? 0.0
      return "Preamp (\(String(format: "%+.1f", gain)) dB)"
    case "invert": return "Phase Invert (180°)"
    case "hi": return "Highshelf Filter"
    case "lo": return "Lowpass Filter"
    case "lo_gain": return "Crossfeed Low Attenuation"
    case "lp": return "Linkwitz-Riley Lowpass (12dB/oct)"
    case "hp": return "Linkwitz-Riley Highpass (12dB/oct)"
    case "conv": return "Convolution IR Engine"
    case "loudness": return "Fader Loudness Compensation"
    case "deemphasis": return "CD De-emphasis Filter"
    case "preemphasis": return "Pre-emphasis Boost"
    case "dcp": return "DC Protection Highpass (7Hz)"
    case "gain": return "Gain (\(String(format: "%+.1f", stage.gainValue)) dB)"
    case "delay": return "Delay (\(String(format: "%.1f", stage.delayValue)) ms)"
    case "volume": return "Volume (\(stage.volumeFader.rawValue))"
    case "lookahead_limiter": return "Lookahead Limiter"
    case "dither": return "Dither (\(stage.ditherBits)-bit)"
    case "diffeq": return "Differential Equation IIR/FIR"
    case "combo": return "Biquad Combo (\(stage.comboType.rawValue))"
    case "limiter": return "Peak Limiter (\(String(format: "%+.1f", stage.limiterLimit)) dB)"
    case "geq": return "Graphic EQ (\(stage.graphicEQBandCount) Bands)"
    default:
      if let bandNum = Int(suffix) {
        return "Biquad Band #\(bandNum)"
      }
      return suffix.capitalized
    }
  }

  private func readableMixerOrProcessorName(_ rawName: String, stage: PipelineStage) -> String {
    if rawName.contains("2to4") {
      return "Expand 2ch ➔ 4ch (Stereo Split)"
    } else if rawName.contains("4to2") {
      return "Sum 4ch ➔ 2ch (Stereo Blend)"
    }

    switch stage.type {
    case .balance:
      return "Balance Mapping (Pos: \(Int(stage.balancePosition * 100))%)"
    case .width:
      return "Stereo Width Matrix (\(stage.widthPercent)%)"
    case .msProc:
      return "Mid/Side Encoding/Decoding Matrix"
    case .mixer:
      return "Matrix Mixer (\(stage.mixerChannelsIn) ➔ \(stage.mixerChannelsOut) ch)"
    case .compressor:
      return "Compressor (Ratio: \(String(format: "%.1f", stage.compressorRatio)):1, Thresh: \(Int(stage.compressorThreshold))dB)"
    case .noiseGate:
      return "Noise Gate (Thresh: \(Int(stage.gateThreshold))dB)"
    case .race:
      return "RACE Spatialization (\(stage.raceDelay)ms)"
    default:
      return rawName
    }
  }
}

// MARK: - Reusable Graph Node Card Container

private struct GraphNodeCard<Content: View>: View {
  let title: String
  let subtitle: String
  let icon: String
  let color: Color
  let isActive: Bool
  var isHovered: Bool = false
  var isWarning: Bool = false
  var onToggleActive: (() -> Void)? = nil
  @ViewBuilder let content: () -> Content

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      // Header
      HStack(spacing: 8) {
        ZStack {
          Circle()
            .fill(isWarning ? Color.red.opacity(0.2) : (isActive ? color.opacity(0.2) : Color.gray.opacity(0.15)))
            .frame(width: 26, height: 26)

          Image(systemName: icon)
            .font(.system(size: 12, weight: .semibold))
            .foregroundStyle(isWarning ? Color.red : (isActive ? color : .secondary))
        }

        VStack(alignment: .leading, spacing: 1) {
          Text(title)
            .font(.system(size: 12, weight: .bold))
            .foregroundStyle(isWarning ? Color.red : Color.primary)
            .lineLimit(1)

          Text(subtitle)
            .font(.system(size: 10))
            .foregroundStyle(isWarning ? Color.red.opacity(0.8) : .secondary)
            .lineLimit(1)
        }

        Spacer(minLength: 4)

        if let onToggleActive = onToggleActive {
          Button(action: onToggleActive) {
            Circle()
              .fill(isActive ? color : Color.gray.opacity(0.3))
              .frame(width: 8, height: 8)
          }
          .buttonStyle(.plain)
          .help(isActive ? "Click to disable stage" : "Click to enable stage")
        }
      }

      // Card Content Body
      content()
    }
    .padding(10)
    .frame(width: 210)
    .background(
      isWarning
        ? Color.red.opacity(0.08)
        : (isActive ? color.opacity(isHovered ? 0.12 : 0.06) : Color.gray.opacity(0.04))
    )
    .cornerRadius(10)
    .overlay(
      RoundedRectangle(cornerRadius: 10)
        .stroke(
          isWarning
            ? Color.red.opacity(0.8)
            : (isActive ? color.opacity(isHovered ? 0.5 : 0.2) : Color.gray.opacity(0.15)),
          lineWidth: isWarning ? 1.5 : (isHovered ? 1.5 : 1.0)
        )
    )
  }
}
