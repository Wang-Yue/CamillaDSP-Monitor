// MiniPlayerView - Compact floating overlay with four display modes
// Appears when user clicks minimize, floats above all windows including fullscreen video

import AppKit
import Observation
import SwiftUI

// MARK: - Mini Player Mode

enum MiniPlayerMode: Int, CaseIterable {
  case spectrum = 0
  case pipeline = 1
  case meters = 2
  case analogVU = 3

  var icon: String {
    switch self {
    case .spectrum: return "waveform.path.ecg.rectangle"
    case .pipeline: return "point.3.connected.trianglepath.dotted"
    case .meters: return "chart.bar"
    case .analogVU: return "gauge.with.needle"
    }
  }
}

// MARK: - Mini Player SwiftUI Content

struct MiniPlayerView: View {
  @Environment(DSPEngineController.self) var dsp
  @Environment(AudioSettings.self) var settings
  @State private var mode: MiniPlayerMode = .spectrum
  @State private var isHovering = false

  var body: some View {
    VStack(spacing: 0) {
      // Header: Controls + switcher (visible on hover)
      HStack(spacing: 6) {
        // Play/Pause toggle
        Button {
          if dsp.status == .inactive {
            dsp.startEngine()
          } else {
            dsp.stopEngine()
          }
        } label: {
          Image(systemName: dsp.status == .inactive ? "play.fill" : "stop.fill")
            .font(.system(size: 10))
            .foregroundStyle(.white.opacity(0.5))
            .frame(width: 18, height: 18)
        }
        .buttonStyle(.plain)

        // Volume Control Row
        HStack(spacing: 6) {
          Button {
            dsp.toggleMute()
          } label: {
            Image(systemName: settings.isMuted ? "speaker.slash.fill" : "speaker.wave.2.fill")
              .font(.system(size: 10))
              .foregroundStyle(settings.isMuted ? .red : .white.opacity(0.5))
              .frame(width: 18, height: 18)
          }
          .buttonStyle(.plain)

          Slider(
            value: Binding(
              get: { settings.volume },
              set: { newValue in
                let rounded = (newValue * 2.0).rounded() / 2.0
                dsp.setVolume(rounded)
              }
            ),
            in: -60...20
          )
          .controlSize(.mini)

          Text(String(format: "%+.0f", settings.volume))
            .font(.system(size: 9, design: .monospaced))
            .foregroundStyle(settings.volume > 0 ? .red : .white.opacity(0.7))
            .frame(width: 25, alignment: .trailing)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 2)
        .opacity(isHovering ? 1 : 0.3)

        // Mode buttons
        ForEach(MiniPlayerMode.allCases, id: \.rawValue) { m in
          Button {
            withAnimation(.easeInOut(duration: 0.15)) { mode = m }
          } label: {
            Image(systemName: m.icon)
              .font(.system(size: 10))
              .foregroundStyle(mode == m ? .white : .white.opacity(0.4))
              .frame(width: 18, height: 18)
          }
          .buttonStyle(.plain)
        }
      }
      .padding(.horizontal, 8)
      .padding(.vertical, 4)
      .opacity(isHovering ? 1 : 0.3)

      // Content
      Group {
        switch mode {
        case .spectrum:
          MiniSpectrumView()
        case .pipeline:
          MiniPipelineView()
        case .meters:
          MiniMetersView()
        case .analogVU:
          MiniAnalogVUView()
        }
      }
      .padding(.horizontal, 8)
      .padding(.bottom, 8)
    }
    .background(
      RoundedRectangle(cornerRadius: 12)
        .fill(.black.opacity(0.45))
    )
    .onHover { hovering in
      withAnimation(.easeInOut(duration: 0.2)) { isHovering = hovering }
    }
    .onTapGesture(count: 2) {
      MiniPlayerWindowController.shared.closeMiniPlayer()
    }
  }
}
