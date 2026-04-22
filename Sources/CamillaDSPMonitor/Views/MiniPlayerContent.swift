// MiniPlayerContent - Mini player content views: spectrum, pipeline, meters

import SwiftUI

// MARK: - Mini Spectrum

struct MiniSpectrumView: View {
  @EnvironmentObject var spectrum: SpectrumEngine

  var body: some View {
    Canvas { context, size in
      drawSpectrumBars(
        context: &context, bands: spectrum.bands,
        maxHeight: size.height, totalWidth: size.width,
        spacing: 1.5, minBarWidth: 2, minBarHeight: 1, cornerRadius: 1)
    }
    .frame(height: 60)
  }
}

// MARK: - Mini Pipeline

struct MiniPipelineView: View {
  @EnvironmentObject var settings: AudioSettings
  @EnvironmentObject var pipeline: PipelineStore

  var body: some View {
    HorizontalScrollWithVerticalWheel {
      HStack(spacing: 3) {
        Button {
          settings.resamplerEnabled.toggle()
        } label: {
          StageChip(
            icon: "arrow.triangle.2.circlepath", label: "Resampler",
            color: .green, isActive: settings.resamplerEnabled, compact: true)
        }
        .buttonStyle(.plain)

        ForEach(pipeline.stages.indices, id: \.self) { index in
          StageChipButton(stage: pipeline.stages[index], compact: true)
        }
      }
    }
    .frame(height: 60)
  }
}

// MARK: - Mini Meters

struct MiniMetersView: View {
  @EnvironmentObject var levels: LevelState

  var body: some View {
    VStack(spacing: 6) {
      MiniMeterRow(label: "L", peak: levels.playbackPeak.left, rms: levels.playbackRms.left)
      MiniMeterRow(label: "R", peak: levels.playbackPeak.right, rms: levels.playbackRms.right)
    }
    .frame(height: 60)
  }
}

struct MiniMeterRow: View {
  let label: String
  let peak: Double
  let rms: Double

  var body: some View {
    HStack(spacing: 6) {
      Text(label)
        .font(.system(size: 10, weight: .medium, design: .monospaced))
        .foregroundStyle(.white.opacity(0.5))
        .frame(width: 12)

      LevelMeterCanvas(peak: peak, rms: rms, compact: true)

      VStack(alignment: .trailing, spacing: 0) {
        Text(String(format: "%5.1f", rms))
          .font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.white.opacity(0.7))
        Text(String(format: "%5.1f", peak))
          .font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.white.opacity(0.4))
      }
      .frame(width: 38)
    }
  }
}
