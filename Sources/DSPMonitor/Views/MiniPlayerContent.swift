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
            spacing: 1.5, minBarWidth: 2, minBarHeight: 1)
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
  var body: some View {
    MiniPipelineOverview()
      .frame(minHeight: 60, maxHeight: .infinity)
  }
}

// MARK: - Mini Meters

struct MiniMetersView: View {
  @Environment(LevelState.self) var levels

  var body: some View {
    let playCount = levels.playbackChannelCount
    VStack(spacing: 6) {
      ForEach(0..<playCount, id: \.self) { ch in
        MiniMeterRow(
          channelIndex: ch,
          label: channelLabel(for: ch, totalCount: playCount)
        )
      }
    }
    .frame(minHeight: 60, maxHeight: .infinity)
    .onAppear { levels.visibilityCount += 1 }
    .onDisappear { levels.visibilityCount -= 1 }
  }

  private func channelLabel(for index: Int, totalCount: Int) -> String {
    "\(index + 1)"
  }
}

struct MiniMeterRow: View {
  let channelIndex: Int
  let label: String

  var body: some View {
    LevelMeterCanvas(
      isPlayback: true,
      channelIndex: channelIndex,
      label: label,
      compact: true
    )
    .frame(height: 18)
  }
}

// MARK: - Mini Analog VU

struct MiniAnalogVUView: View {
  @Environment(LevelState.self) var levels
  @Environment(VUSettings.self) var vuSettings

  var body: some View {
    let playCount = levels.playbackChannelCount
    GeometryReader { geometry in
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 8) {
          ForEach(0..<playCount, id: \.self) { ch in
            AnalogVUMeter(
              isPlayback: true,
              channelIndex: ch,
              label: channelLabel(for: ch, totalCount: playCount),
              params: vuSettings.params
            )
          }
        }
      }
    }
    .frame(minHeight: 60, maxHeight: .infinity)
    .onAppear { levels.visibilityCount += 1 }
    .onDisappear { levels.visibilityCount -= 1 }
  }

  private func channelLabel(for index: Int, totalCount: Int) -> String {
    "\(index + 1)"
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
