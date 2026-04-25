// VolumeControlView - Volume slider with mute button

import Observation
import SwiftUI

struct VolumeControlView: View {
  @Environment(DSPEngineController.self) var dsp
  @Environment(AudioSettings.self) var settings

  var body: some View {
    HStack(spacing: 8) {
      Button {
        dsp.toggleMute()
      } label: {
        Image(systemName: settings.isMuted ? "speaker.slash.fill" : "speaker.wave.2.fill")
          .foregroundStyle(settings.isMuted ? .red : .primary)
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
      .frame(width: 200)

      Text(String(format: "%+.1f dB", settings.volume))
        .font(.system(.caption, design: .monospaced))
        .foregroundStyle(settings.volume > 0 ? .red : .primary)
        .frame(width: 65, alignment: .trailing)
    }
  }
}
