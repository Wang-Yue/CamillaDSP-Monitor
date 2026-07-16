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
    let deltaX = event.hasPreciseScrollingDeltas ? event.scrollingDeltaX : event.deltaX
    let deltaY = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY : event.deltaY

    if abs(deltaX) >= abs(deltaY) {
      super.scrollWheel(with: event)
    } else {
      guard let documentView = documentView else {
        super.scrollWheel(with: event)
        return
      }
      let scrollDistance = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY : (event.deltaY * 10)
      let maxScrollX = max(0, documentView.frame.width - contentView.bounds.width)
      var newOrigin = contentView.bounds.origin
      newOrigin.x = max(0, min(newOrigin.x - scrollDistance, maxScrollX))
      contentView.scroll(to: newOrigin)
      reflectScrolledClipView(contentView)
    }
  }
}

struct DashboardView: View {
  @Environment(AppState.self) var appState

  var body: some View {
    ScrollView {
      LazyVStack(spacing: 20) {
        if appState.showSignalGraphInDashboard {
          PipelineOverviewCard()
          DSPDetailedSignalGraphCard()
        }

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
    "\(index + 1)"
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
    @Bindable var bindableSettings = settings
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
        value: $bindableSettings[faderVolume: fader],
        in: -60...20
      )
      .onChange(of: settings.volume(for: fader)) { _, newVol in
        let rounded = (newVol * 2.0).rounded() / 2.0
        dsp.setFaderVolume(fader: fader, db: rounded)
      }

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
