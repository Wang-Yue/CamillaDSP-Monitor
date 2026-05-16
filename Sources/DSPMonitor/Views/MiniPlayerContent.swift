// MiniPlayerContent - Mini player content views: spectrum, pipeline, meters

import Observation
import SwiftUI

// MARK: - Mini Spectrum

struct MiniSpectrumView: View {
  @Environment(SpectrumEngine.self) var spectrum

  var body: some View {
    ZStack {
      if let bands = spectrum.bands {
        Canvas { context, size in
          drawSpectrumBars(
            context: &context, bands: bands,
            maxHeight: size.height, totalWidth: size.width,
            spacing: 1.5, minBarWidth: 2, minBarHeight: 1, cornerRadius: 1)
        }
      }
    }
    .frame(minHeight: 60, maxHeight: .infinity)
    .onAppear { spectrum.visibilityCount += 1 }
    .onDisappear { spectrum.visibilityCount -= 1 }
  }
}

// MARK: - Mini Pipeline

struct MiniPipelineView: View {
  @Environment(AudioSettings.self) var settings
  @Environment(PipelineStore.self) var pipeline

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
    .frame(minHeight: 60, maxHeight: .infinity)
  }
}

// MARK: - Mini Meters

struct MiniMetersView: View {
  @Environment(LevelState.self) var levels

  var body: some View {
    VStack(spacing: 6) {
      MiniMeterRow(label: "L", peak: levels.playbackPeak.left, rms: levels.playbackRms.left)
      MiniMeterRow(label: "R", peak: levels.playbackPeak.right, rms: levels.playbackRms.right)
    }
    .frame(minHeight: 60, maxHeight: .infinity)
    .onAppear { levels.visibilityCount += 1 }
    .onDisappear { levels.visibilityCount -= 1 }
  }
}

struct MiniMeterRow: View {
  let label: String
  let peak: Float
  let rms: Float

  var body: some View {
    HStack(spacing: 6) {
      Text(label)
        .font(.system(size: 10, weight: .medium, design: .monospaced))
        .foregroundStyle(.white.opacity(0.5))
        .frame(width: 12)
        .fixedSize()

      LevelMeterCanvas(peak: peak, rms: rms, compact: true)

      VStack(alignment: .trailing, spacing: 0) {
        Text(String(format: "%5.1f", rms))
          .font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.white.opacity(0.7))
          .fixedSize()
        Text(String(format: "%5.1f", peak))
          .font(.system(size: 9, design: .monospaced))
          .foregroundStyle(.white.opacity(0.4))
          .fixedSize()
      }
      .frame(width: 38)
    }
  }
}

// MARK: - Mini Analog VU

struct MiniAnalogVUView: View {
  @Environment(LevelState.self) var levels
  @Environment(VUSettings.self) var vuSettings

  var body: some View {
    GeometryReader { geometry in
      HStack(spacing: 8) {
        AnalogVUMeter(
          level: levels.playbackRms.left, label: "L", params: vuSettings.params,
          height: geometry.size.height)
        AnalogVUMeter(
          level: levels.playbackRms.right, label: "R", params: vuSettings.params,
          height: geometry.size.height)
      }
    }
    .frame(minHeight: 60, maxHeight: .infinity)
    .onAppear { levels.visibilityCount += 1 }
    .onDisappear { levels.visibilityCount -= 1 }
  }
}

// MARK: - Mini Spectrogram

struct MiniSpectrogramView: View {
  @Environment(SpectrogramEngine.self) var spectroscope

  var body: some View {
    ZStack {
      SpectrogramContentView(leftPadding: 0, bottomPadding: 0)
    }
    .frame(minHeight: 60, maxHeight: .infinity)
    .onAppear { spectroscope.visibilityCount += 1 }
    .onDisappear { spectroscope.visibilityCount -= 1 }
  }
}

// MARK: - Mini Vector Scope

struct MiniVectorScopeView: View {
  @Environment(VectorScopeEngine.self) var vectorscope

  var body: some View {
    ZStack {
      VectorScopeContentView(showGrid: false)
    }
    .frame(minHeight: 60, maxHeight: .infinity)
    .onAppear { vectorscope.visibilityCount += 1 }
    .onDisappear { vectorscope.visibilityCount -= 1 }
  }
}
