// DashboardView - Main dashboard showing pipeline overview and monitoring

import AppKit
import SwiftUI

/// Horizontal ScrollView that also scrolls with vertical mouse wheel.
struct HorizontalScrollWithVerticalWheel<Content: View>: NSViewRepresentable {
  let content: Content

  init(@ViewBuilder content: () -> Content) {
    self.content = content()
  }

  func makeNSView(context: Context) -> NSScrollView {
    let scrollView = VerticalToHorizontalScrollView()
    let hostingView = NSHostingView(rootView: content)
    hostingView.translatesAutoresizingMaskIntoConstraints = false

    scrollView.documentView = hostingView
    scrollView.hasHorizontalScroller = false
    scrollView.hasVerticalScroller = false
    scrollView.drawsBackground = false

    NSLayoutConstraint.activate([
      hostingView.topAnchor.constraint(equalTo: scrollView.contentView.topAnchor),
      hostingView.leadingAnchor.constraint(equalTo: scrollView.contentView.leadingAnchor),
      hostingView.heightAnchor.constraint(equalTo: scrollView.contentView.heightAnchor),
    ])

    return scrollView
  }

  func updateNSView(_ nsView: NSScrollView, context: Context) {
    if let hostingView = nsView.documentView as? NSHostingView<Content> {
      hostingView.rootView = content
    }
  }
}

private class VerticalToHorizontalScrollView: NSScrollView {
  override func scrollWheel(with event: NSEvent) {
    if abs(event.deltaX) >= abs(event.deltaY) {
      super.scrollWheel(with: event)
    } else {
      // Convert vertical scroll to horizontal
      let converted = NSEvent.init(
        cgEvent: {
          let cg = event.cgEvent!
          cg.setDoubleValueField(
            .scrollWheelEventDeltaAxis2, value: cg.getDoubleValueField(.scrollWheelEventDeltaAxis1))
          cg.setDoubleValueField(.scrollWheelEventDeltaAxis1, value: 0)
          return cg
        }()
      )!
      super.scrollWheel(with: converted)
    }
  }
}

struct DashboardView: View {
  @EnvironmentObject var appState: AppState

  var body: some View {
    ScrollView {
      VStack(spacing: 20) {
        PipelineOverview()
        LevelMetersCard()
        SpectrumCard()
      }
      .padding()
    }
    .background(Color(nsColor: .controlBackgroundColor))
  }
}

struct PipelineOverview: View {
  @EnvironmentObject var appState: AppState

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Signal Chain").font(.headline)
      HorizontalScrollWithVerticalWheel {
        HStack(spacing: 4) {
          StageChip(
            icon: "mic", label: appState.selectedCaptureDevice ?? "Input", color: .blue,
            isActive: appState.isRunning)
          Image(systemName: "chevron.right").foregroundStyle(.tertiary).font(.caption)
          Button {
            appState.resamplerEnabled.toggle()
          } label: {
            StageChip(
              icon: "arrow.triangle.2.circlepath", label: "Resampler",
              color: appState.resamplerEnabled ? Color.accentColor : .gray,
              isActive: appState.resamplerEnabled)
          }.buttonStyle(.plain)
          Image(systemName: "chevron.right").foregroundStyle(.tertiary).font(.caption)
          ForEach(appState.stages) { stage in
            DashboardStageChipButton(stage: stage)
            Image(systemName: "chevron.right").foregroundStyle(.tertiary).font(.caption)
          }
          StageChip(
            icon: "hifispeaker", label: appState.selectedPlaybackDevice ?? "Output", color: .green,
            isActive: appState.isRunning)
        }.padding(.vertical, 4)
      }
    }.padding().background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
  }
}

struct StageChip: View {
  let icon: String
  let label: String
  let color: Color
  let isActive: Bool
  var body: some View {
    HStack(spacing: 6) {
      Image(systemName: icon).font(.caption)
      Text(label).font(.caption).lineLimit(1)
    }
    .padding(.horizontal, 10).padding(.vertical, 6).background(
      isActive ? color.opacity(0.15) : Color.gray.opacity(0.08)
    ).foregroundStyle(isActive ? color : .secondary)
    .onHover { h in if h { NSCursor.pointingHand.push() } else { NSCursor.pop() } }
    .clipShape(Capsule()).overlay(
      Capsule().stroke(isActive ? color.opacity(0.3) : Color.clear, lineWidth: 1))
  }
}

private struct DashboardStageChipButton: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var appState: AppState
  var body: some View {
    Button {
      stage.isEnabled.toggle()
      appState.applyConfig()
    } label: {
      StageChip(
        icon: stage.type.icon, label: stage.name,
        color: stage.isEnabled ? Color.accentColor : .gray, isActive: stage.isEnabled)
    }.buttonStyle(.plain)
  }
}

struct LevelMetersCard: View {
  @EnvironmentObject var levels: LevelState
  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack(alignment: .firstTextBaseline) {
        Text("Levels").font(.headline)
        Spacer()
        Text("RMS / Peak").font(.caption).foregroundStyle(.tertiary)
      }
      HStack(spacing: 24) {
        VStack(alignment: .leading, spacing: 8) {
          Text("Capture").font(.subheadline).foregroundStyle(.secondary)
          DualLevelMeterView(
            label: "L", peak: levels.capturePeak.left, rms: levels.captureRms.left)
          DualLevelMeterView(
            label: "R", peak: levels.capturePeak.right, rms: levels.captureRms.right)
        }
        VStack(alignment: .leading, spacing: 8) {
          Text("Playback").font(.subheadline).foregroundStyle(.secondary)
          DualLevelMeterView(
            label: "L", peak: levels.playbackPeak.left, rms: levels.playbackRms.left)
          DualLevelMeterView(
            label: "R", peak: levels.playbackPeak.right, rms: levels.playbackRms.right)
        }
      }
    }.padding().background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
  }
}

struct SpectrumCard: View {
  @EnvironmentObject var spectrum: SpectrumState
  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Text("Spectrum").font(.headline)
        Spacer()
        Text("FFT Pre-Processing").font(.caption).foregroundStyle(.tertiary)
      }
      SpectrumView(bands: spectrum.bands).frame(height: 160)
    }.padding().background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
  }
}
