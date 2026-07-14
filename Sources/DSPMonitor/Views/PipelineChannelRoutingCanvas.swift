// PipelineChannelRoutingCanvas.swift - 2D Detailed Audio Channel Routing & Block Canvas View

import DSPConfig
import Observation
import SwiftUI

struct PipelineChannelRoutingCanvas: View {
  @Environment(AudioDeviceManager.self) var devices
  @Environment(AudioSettings.self) var settings
  @Environment(PipelineStore.self) var pipeline

  @State private var hoveredChannel: Int? = nil
  @State private var hoveredStepID: String? = nil

  private var captureChannels: Int { devices.captureConfig.channels }
  private var playbackChannels: Int { devices.playbackConfig.channels }
  private var sampleRate: Int { devices.captureConfig.sampleRate }

  // MARK: - Routing Step Node Representation

  private struct RoutingStepItem: Identifiable {
    let id: String
    let stage: PipelineStage
    let step: PipelineStep
    let stepIndex: Int
    let incomingChannels: Int
    let outgoingChannels: Int
    let targetChannels: Set<Int>
    let title: String
    let subtitle: String
  }

  // MARK: - Unroll Pipeline into Column Steps

  private var stepItems: [RoutingStepItem] {
    var items: [RoutingStepItem] = []
    var currentCh = captureChannels

    for (stageIdx, stage) in pipeline.stages.enumerated() {
      guard stage.isEnabled && stage.isActive else { continue }
      let inCh = pipeline.channelCount(beforeStageAtIndex: stageIdx, captureChannels: captureChannels)
      let steps = stage.buildPipelineSteps(
        channelCount: inCh,
        eqPresets: pipeline.eqPresets,
        convPresets: pipeline.convPresets,
        sampleRate: sampleRate
      )

      currentCh = inCh
      for (stepIdx, step) in steps.enumerated() {
        var outCh = currentCh
        if step.type == .mixer {
          outCh = stage.mixerChannelsOut
        }

        let targetChs: Set<Int> = {
          if let chs = step.channels {
            return Set(chs)
          } else if let ch = step.channel {
            return [ch]
          } else if step.type == .processor {
            return stage.channels
          } else {
            return Set(0..<currentCh)
          }
        }()

        let title: String = {
          switch step.type {
          case .filter: return stage.name
          case .mixer: return "\(stage.name) (Mixer)"
          case .processor: return stage.name
          }
        }()

        let subtitle: String = {
          if let names = step.names, !names.isEmpty {
            return names.map { readableFilterSuffix($0) }.joined(separator: ", ")
          } else if let name = step.name {
            return readableMixerOrProcessorName(name, stage: stage)
          }
          return stage.type.rawValue
        }()

        let itemID = "\(stage.id.uuidString)_\(stepIdx)"
        items.append(
          RoutingStepItem(
            id: itemID,
            stage: stage,
            step: step,
            stepIndex: stepIdx,
            incomingChannels: currentCh,
            outgoingChannels: outCh,
            targetChannels: targetChs,
            title: title,
            subtitle: subtitle
          )
        )
        currentCh = outCh
      }
    }
    return items
  }

  private var maxChannels: Int {
    var maxCh = max(captureChannels, playbackChannels)
    for item in stepItems {
      maxCh = max(maxCh, item.incomingChannels, item.outgoingChannels)
    }
    return max(maxCh, 2)
  }

  private var trackHeight: CGFloat { 48 }
  private var columnWidth: CGFloat { 180 }
  private var connectorWidth: CGFloat { 50 }
  private var canvasHeight: CGFloat { CGFloat(maxChannels) * trackHeight + 40 }

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      // Legend / Channel Tracks Bar
      channelLegendBar

      // 2D Scrollable Graph Canvas
      HorizontalScrollWithVerticalWheel {
        ZStack(alignment: .topLeading) {
          // Layer 1: Background Wire Track Paths (Canvas)
          wireCanvasLayer

          // Layer 2: Interactive Step Blocks & Column Headers
          HStack(alignment: .top, spacing: 0) {
            // Left Input Channel Ports Column
            inputPortsColumn

            // Connector: Input to First Step
            connectorSpacer(fromCh: captureChannels, toCh: firstStepInputChannels)

            // Resampler (if active)
            if settings.resamplerEnabled {
              resamplerBlock
              connectorSpacer(fromCh: captureChannels, toCh: firstStepInputChannels)
            }

            // Step Columns
            ForEach(Array(stepItems.enumerated()), id: \.element.id) { idx, item in
              let nextInCh = (idx + 1 < stepItems.count)
                ? stepItems[idx + 1].incomingChannels
                : playbackChannels

              HStack(alignment: .top, spacing: 0) {
                stepBlockView(item: item)
                connectorSpacer(fromCh: item.outgoingChannels, toCh: nextInCh, isMismatch: isStepMismatch(item: item, nextInCh: nextInCh, isLast: idx == stepItems.count - 1))
              }
            }

            if stepItems.isEmpty && !settings.resamplerEnabled {
              connectorSpacer(fromCh: captureChannels, toCh: playbackChannels, isMismatch: captureChannels != playbackChannels)
            }

            // Right Output Channel Ports Column
            outputPortsColumn
          }
          .padding(.vertical, 20)
          .padding(.horizontal, 10)
        }
        .frame(minHeight: canvasHeight)
      }
    }
  }

  private var firstStepInputChannels: Int {
    stepItems.first?.incomingChannels ?? captureChannels
  }

  private func isStepMismatch(item: RoutingStepItem, nextInCh: Int, isLast: Bool) -> Bool {
    if isLast {
      return item.outgoingChannels != playbackChannels
    }
    return item.outgoingChannels != nextInCh
  }

  // MARK: - Channel Legend Bar

  private var channelLegendBar: some View {
    HStack(spacing: 12) {
      Text("Channel Signal Wires:")
        .font(.caption2.bold())
        .foregroundStyle(.secondary)

      ForEach(0..<maxChannels, id: \.self) { ch in
        HStack(spacing: 4) {
          Circle()
            .fill(channelColor(ch))
            .frame(width: 8, height: 8)
          Text("Ch \(ch + 1)")
            .font(.system(size: 10, weight: .bold, design: .monospaced))
            .foregroundStyle(hoveredChannel == ch ? channelColor(ch) : .secondary)
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 3)
        .background(hoveredChannel == ch ? channelColor(ch).opacity(0.15) : Color.primary.opacity(0.04))
        .cornerRadius(4)
        .onHover { h in hoveredChannel = h ? ch : nil }
      }
    }
    .padding(.horizontal, 4)
  }

  // MARK: - Wire Paths Canvas Layer

  private var wireCanvasLayer: some View {
    Canvas { context, size in
      // Draw continuous horizontal wire tracks for all channels
      for ch in 0..<maxChannels {
        let y = CGFloat(ch) * trackHeight + 45
        let isHighlighted = hoveredChannel == nil || hoveredChannel == ch

        var path = Path()
        path.move(to: CGPoint(x: 10, y: y))
        path.addLine(to: CGPoint(x: size.width - 10, y: y))

        context.stroke(
          path,
          with: .color(channelColor(ch).opacity(isHighlighted ? 0.45 : 0.12)),
          lineWidth: isHighlighted ? 2.5 : 1.0
        )
      }
    }
  }

  // MARK: - Input Ports Column

  private var inputPortsColumn: some View {
    VStack(alignment: .leading, spacing: 0) {
      Text("CAPTURE INPUT")
        .font(.system(size: 9, weight: .bold, design: .monospaced))
        .foregroundStyle(.secondary)
        .padding(.bottom, 6)

      ZStack(alignment: .topLeading) {
        VStack(spacing: 0) {
          ForEach(0..<captureChannels, id: \.self) { ch in
            HStack(spacing: 6) {
              Circle()
                .fill(channelColor(ch))
                .frame(width: 10, height: 10)

              Text("Ch \(ch + 1)")
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundStyle(channelColor(ch))

              Spacer()
            }
            .padding(.horizontal, 8)
            .frame(height: trackHeight)
            .background(channelColor(ch).opacity(0.12))
            .cornerRadius(6)
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(channelColor(ch).opacity(0.3), lineWidth: 1))
          }
        }
      }
    }
    .frame(width: 110)
  }

  // MARK: - Output Ports Column

  private var outputPortsColumn: some View {
    VStack(alignment: .leading, spacing: 0) {
      Text("PLAYBACK OUTPUT")
        .font(.system(size: 9, weight: .bold, design: .monospaced))
        .foregroundStyle(.secondary)
        .padding(.bottom, 6)

      VStack(spacing: 0) {
        ForEach(0..<playbackChannels, id: \.self) { ch in
          HStack(spacing: 6) {
            Text("Ch \(ch + 1)")
              .font(.system(size: 11, weight: .bold, design: .monospaced))
              .foregroundStyle(channelColor(ch))

            Spacer()

            Circle()
              .fill(channelColor(ch))
              .frame(width: 10, height: 10)
          }
          .padding(.horizontal, 8)
          .frame(height: trackHeight)
          .background(channelColor(ch).opacity(0.12))
          .cornerRadius(6)
          .overlay(RoundedRectangle(cornerRadius: 6).stroke(channelColor(ch).opacity(0.3), lineWidth: 1))
        }
      }
    }
    .frame(width: 110)
  }

  // MARK: - Resampler Block

  private var resamplerBlock: some View {
    VStack(alignment: .leading, spacing: 0) {
      Text("RESAMPLER")
        .font(.system(size: 9, weight: .bold, design: .monospaced))
        .foregroundStyle(Color.accentColor)
        .padding(.bottom, 6)

      VStack(alignment: .leading, spacing: 4) {
        HStack(spacing: 6) {
          Image(systemName: "arrow.triangle.2.circlepath")
            .font(.caption)
          Text("SRC Resampler")
            .font(.system(size: 11, weight: .bold))
        }

        Text("Applies to all \(captureChannels) channels")
          .font(.system(size: 9))
          .foregroundStyle(.secondary)
      }
      .padding(8)
      .frame(width: columnWidth, height: CGFloat(captureChannels) * trackHeight, alignment: .topLeading)
      .background(Color.accentColor.opacity(0.08))
      .cornerRadius(8)
      .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.accentColor.opacity(0.4), lineWidth: 1))
    }
  }

  // MARK: - Step Block Node View

  private func stepBlockView(item: RoutingStepItem) -> some View {
    let minCh = item.targetChannels.min() ?? 0
    let maxCh = item.targetChannels.max() ?? (item.incomingChannels - 1)
    let topYOffset = CGFloat(minCh) * trackHeight
    let blockHeight = max(trackHeight, CGFloat(maxCh - minCh + 1) * trackHeight)
    let color = stepTypeColor(item.step.type)
    let isHovered = hoveredStepID == item.id

    return VStack(alignment: .leading, spacing: 0) {
      Text(item.title.uppercased())
        .font(.system(size: 9, weight: .bold, design: .monospaced))
        .foregroundStyle(color)
        .padding(.bottom, 6)

      ZStack(alignment: .topLeading) {
        // Space placeholders for non-targeted channels above block
        Color.clear
          .frame(height: CGFloat(maxChannels) * trackHeight)

        // The active building block card positioned precisely across target channels
        VStack(alignment: .leading, spacing: 6) {
          // Block Header
          HStack(spacing: 6) {
            stepTypeIcon(item.step.type)
              .font(.caption)
              .foregroundStyle(color)

            Text(item.title)
              .font(.system(size: 11, weight: .bold))
              .lineLimit(1)

            Spacer()

            // Active channels indicator badge
            HStack(spacing: 2) {
              ForEach(item.targetChannels.sorted(), id: \.self) { ch in
                Text("\(ch + 1)")
                  .font(.system(size: 8, weight: .bold))
                  .padding(.horizontal, 3)
                  .padding(.vertical, 1)
                  .background(channelColor(ch).opacity(0.3))
                  .foregroundStyle(channelColor(ch))
                  .cornerRadius(3)
              }
            }
          }

          // Subtitle / Filter breakdown details
          Text(item.subtitle)
            .font(.system(size: 9, design: .monospaced))
            .foregroundStyle(.secondary)
            .lineLimit(2)
        }
        .padding(8)
        .frame(width: columnWidth, height: blockHeight, alignment: .topLeading)
        .background(color.opacity(isHovered ? 0.16 : 0.08))
        .cornerRadius(8)
        .overlay(
          RoundedRectangle(cornerRadius: 8)
            .stroke(color.opacity(isHovered ? 0.8 : 0.4), lineWidth: isHovered ? 1.5 : 1.0)
        )
        .offset(y: topYOffset)
        .onHover { h in
          hoveredStepID = h ? item.id : nil
        }
      }
    }
  }

  // MARK: - Connector Spacer / Mismatch Arrow

  private func connectorSpacer(fromCh: Int, toCh: Int, isMismatch: Bool = false) -> some View {
    VStack(spacing: 4) {
      Spacer()
      HStack(spacing: 2) {
        Rectangle()
          .fill(
            LinearGradient(
              colors: isMismatch
                ? [Color.red.opacity(0.6), Color.red]
                : [Color.accentColor.opacity(0.4), Color.accentColor.opacity(0.8)],
              startPoint: .leading,
              endPoint: .trailing
            )
          )
          .frame(width: connectorWidth - 14, height: 2)

        Image(systemName: isMismatch ? "exclamationmark.triangle.fill" : "chevron.right")
          .font(.system(size: isMismatch ? 9 : 8, weight: .bold))
          .foregroundStyle(isMismatch ? Color.red : Color.accentColor.opacity(0.8))
      }

      Text(isMismatch ? "\(fromCh) ch ❌" : "\(fromCh)\(fromCh != toCh ? "➔\(toCh)" : "") ch")
        .font(.system(size: 8, weight: .bold, design: .monospaced))
        .padding(.horizontal, 4)
        .padding(.vertical, 1)
        .background(isMismatch ? Color.red.opacity(0.15) : Color.primary.opacity(0.05))
        .foregroundStyle(isMismatch ? Color.red : .secondary)
        .cornerRadius(3)
      Spacer()
    }
    .frame(width: connectorWidth)
  }

  // MARK: - Color Resolvers

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

  private func readableFilterSuffix(_ rawName: String) -> String {
    let parts = rawName.components(separatedBy: "_")
    guard let suffix = parts.last else { return rawName }

    switch suffix {
    case "preamp": return "Preamp"
    case "invert": return "Phase Invert"
    case "hi": return "Highshelf"
    case "lo": return "Lowpass"
    case "lo_gain": return "Crossfeed Atten"
    case "lp": return "Linkwitz Lowpass"
    case "hp": return "Linkwitz Highpass"
    case "conv": return "Convolution IR"
    case "loudness": return "Loudness"
    case "deemphasis": return "De-emphasis"
    case "preemphasis": return "Pre-emphasis"
    case "dcp": return "DC Protection"
    case "gain": return "Gain"
    case "delay": return "Delay"
    case "volume": return "Volume"
    case "lookahead_limiter": return "Lookahead Limiter"
    case "dither": return "Dither"
    case "diffeq": return "DiffEq"
    case "combo": return "Biquad Combo"
    case "limiter": return "Limiter"
    case "geq": return "Graphic EQ"
    default:
      if let b = Int(suffix) { return "Band #\(b)" }
      return suffix.capitalized
    }
  }

  private func readableMixerOrProcessorName(_ rawName: String, stage: PipelineStage) -> String {
    if rawName.contains("2to4") {
      return "Expand 2ch ➔ 4ch"
    } else if rawName.contains("4to2") {
      return "Sum 4ch ➔ 2ch"
    }
    return stage.type.rawValue
  }
}
