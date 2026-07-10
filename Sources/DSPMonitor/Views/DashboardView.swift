// DashboardView - Main dashboard showing pipeline overview and monitoring

import AppKit
import DSPConfig
import Observation
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

    // Disable all bouncing to prevent tiny up/down "wiggles"
    scrollView.horizontalScrollElasticity = .none
    scrollView.verticalScrollElasticity = .none

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
      guard let cg = event.cgEvent else {
        super.scrollWheel(with: event)
        return
      }
      cg.setDoubleValueField(
        .scrollWheelEventDeltaAxis2,
        value: cg.getDoubleValueField(.scrollWheelEventDeltaAxis1)
      )
      cg.setDoubleValueField(.scrollWheelEventDeltaAxis1, value: 0)
      if let converted = NSEvent(cgEvent: cg) {
        super.scrollWheel(with: converted)
      } else {
        super.scrollWheel(with: event)
      }
    }
  }
}

struct DashboardView: View {
  @Environment(AppState.self) var appState

  var body: some View {
    ScrollView {
      VStack(spacing: 20) {
        PipelineOverview()

        if appState.showLevelMetersInDashboard {
          LevelMetersCard()
        }
        FadersCard()
        if appState.showAnalogVUInDashboard {
          AnalogVUCard()
        }
        if appState.showSpectrumInDashboard {
          SpectrumCard()
        }
        if appState.showSpectrogramInDashboard {
          SpectrogramCard()
        }
        if appState.showVectorScopeInDashboard {
          VectorScopeView()
            .frame(height: 700)
        }
      }
      .padding()
    }
    .background(Color(nsColor: .controlBackgroundColor))
  }
}

struct PipelineOverview: View {
  @Environment(DSPEngineController.self) var dsp
  @Environment(AudioDeviceManager.self) var devices
  @Environment(AudioSettings.self) var settings
  @Environment(PipelineStore.self) var pipeline

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Signal Chain").font(.headline)
      HorizontalScrollWithVerticalWheel {
        HStack(spacing: 4) {
          StageChip(
            icon: "mic", label: devices.captureConfig.deviceName ?? "Input", color: .blue,
            isActive: dsp.status == .running)
          Image(systemName: "chevron.right").foregroundStyle(.tertiary).font(.caption)
          Button {
            settings.resamplerEnabled.toggle()
          } label: {
            StageChip(
              icon: "arrow.triangle.2.circlepath", label: "Resampler",
              color: settings.resamplerEnabled ? Color.accentColor : .gray,
              isActive: settings.resamplerEnabled)
          }.buttonStyle(.plain)
          Image(systemName: "chevron.right").foregroundStyle(.tertiary).font(.caption)
          ForEach(pipeline.stages) { stage in
            StageChipButton(stage: stage)
            Image(systemName: "chevron.right").foregroundStyle(.tertiary).font(.caption)
          }
          StageChip(
            icon: "hifispeaker", label: devices.playbackConfig.deviceName ?? "Output",
            color: .green, isActive: dsp.status == .running)
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

struct LevelMetersCard: View {
  @Environment(LevelState.self) var levels
  var body: some View {
    let captureCount = levels.captureChannelCount
    let playbackCount = levels.playbackChannelCount

    VStack(alignment: .leading, spacing: 12) {
      HStack(alignment: .firstTextBaseline) {
        Text("Levels").font(.headline)
        Spacer()
        Text("RMS / Peak").font(.caption).foregroundStyle(.tertiary)
      }
      HStack(spacing: 24) {
        VStack(alignment: .leading, spacing: 8) {
          Text("Capture").font(.subheadline).foregroundStyle(.secondary)
          ForEach(0..<captureCount, id: \.self) { ch in
            DualLevelMeterView(
              isPlayback: false,
              channelIndex: ch,
              label: channelLabel(for: ch, totalCount: captureCount)
            )
          }
        }
        VStack(alignment: .leading, spacing: 8) {
          Text("Playback").font(.subheadline).foregroundStyle(.secondary)
          ForEach(0..<playbackCount, id: \.self) { ch in
            DualLevelMeterView(
              isPlayback: true,
              channelIndex: ch,
              label: channelLabel(for: ch, totalCount: playbackCount)
            )
          }
        }
      }
    }
    .padding()
    .background(Color.primary.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    .onAppear { levels.visibilityCount += 1 }
    .onDisappear { levels.visibilityCount -= 1 }
  }

  private func channelLabel(for index: Int, totalCount: Int) -> String {
    if totalCount == 2 {
      return index == 0 ? "L" : "R"
    }
    if index == 0 { return "L" }
    if index == 1 { return "R" }
    return "\(index + 1)"
  }
}

struct SpectrumCard: View {
  @Environment(SpectrumEngine.self) var spectrum

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Spectrum").font(.headline)
      SpectrumView(bands: spectrum.bands, frequencies: spectrum.frequencies).frame(height: 160)
    }
    .padding()
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
    .onAppear { spectrum.visibilityCount += 1 }
    .onDisappear { spectrum.visibilityCount -= 1 }
  }
}

struct SpectrogramCard: View {
  @Environment(SpectrogramEngine.self) var spectroscope

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Spectroscope").font(.headline)
      SpectrogramView()
        .frame(height: 480)
        .cornerRadius(8)
    }
    .padding()
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
    .onAppear { spectroscope.visibilityCount += 1 }
    .onDisappear { spectroscope.visibilityCount -= 1 }
  }
}

// MARK: - Faders Card

struct FadersCard: View {
  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      Text("Volume Faders").font(.headline)
      VStack(spacing: 12) {
        ForEach(Fader.allCases, id: \.self) { fader in
          FaderControlView(fader: fader)
        }
      }
    }
    .padding()
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
  }
}

struct FaderControlView: View {
  let fader: Fader
  @Environment(DSPEngineController.self) var dsp
  @Environment(AudioSettings.self) var settings

  var body: some View {
    HStack(spacing: 12) {
      Text(faderName(fader))
        .frame(width: 80, alignment: .leading)
        .font(.subheadline)

      Button {
        dsp.toggleFaderMute(fader: fader)
      } label: {
        Image(
          systemName: settings.isMuted(for: fader) ? "speaker.slash.fill" : "speaker.wave.2.fill"
        )
        .foregroundStyle(settings.isMuted(for: fader) ? .red : .primary)
        .frame(width: 20)
      }
      .buttonStyle(.plain)

      Slider(
        value: Binding(
          get: { settings.volume(for: fader) },
          set: { newValue in
            let rounded = (newValue * 2.0).rounded() / 2.0
            dsp.setFaderVolume(fader: fader, db: rounded)
          }
        ),
        in: -60...20
      )

      Text(String(format: "%+.1f dB", settings.volume(for: fader)))
        .font(.system(.body, design: .monospaced))
        .foregroundStyle(settings.volume(for: fader) > 0 ? .red : .primary)
        .frame(width: 70, alignment: .trailing)
    }
  }

  private func faderName(_ fader: Fader) -> String {
    switch fader {
    case .main: return "Main"
    case .aux1: return "Aux 1"
    case .aux2: return "Aux 2"
    case .aux3: return "Aux 3"
    case .aux4: return "Aux 4"
    }
  }
}
