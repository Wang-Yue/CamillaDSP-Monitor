// VolumeControlView - Volume slider with mute button

import SwiftUI

struct VolumeControlView: View {
  @EnvironmentObject var appState: AppState

  var body: some View {
    HStack(spacing: 8) {
      // Mute button
      Button {
        appState.toggleMute()
      } label: {
        Image(systemName: appState.isMuted ? "speaker.slash.fill" : "speaker.wave.2.fill")
          .foregroundStyle(appState.isMuted ? .red : .primary)
      }
      .buttonStyle(.plain)

      // Volume slider
      Slider(
        value: Binding(
          get: { appState.volume },
          set: { appState.setVolume($0) }
        ),
        in: -60...20,
        step: 0.5
      )
      .frame(width: 200)

      // Volume readout
      Text(String(format: "%+.1f dB", appState.volume))
        .font(.system(.caption, design: .monospaced))
        .foregroundStyle(appState.volume > 0 ? .red : .primary)
        .frame(width: 65, alignment: .trailing)
    }
  }
}
