// MiniPlayerContent - Mini player content views: spectrum, pipeline, meters

import SwiftUI

// MARK: - Mini Spectrum

struct MiniSpectrumView: View {
  @EnvironmentObject var spectrum: SpectrumState

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
  @EnvironmentObject var appState: AppState

  var body: some View {
    HorizontalScrollWithVerticalWheel {
      HStack(spacing: 3) {
        // Resampler chip
        Button {
          appState.resamplerEnabled.toggle()
        } label: {
          StageChip(
            icon: "arrow.triangle.2.circlepath", label: "Resampler",
            color: .green, isActive: appState.resamplerEnabled, compact: true)
        }
        .buttonStyle(.plain)

        // Pipeline stages — each in its own view so @ObservedObject triggers redraws
        ForEach(appState.stages.indices, id: \.self) { index in
          StageChipButton(stage: appState.stages[index], compact: true)
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

      // dB values
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
