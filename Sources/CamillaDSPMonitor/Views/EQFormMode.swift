// EQFormMode - Simplified list view for editing EQ bands

import SwiftUI

struct EQFormMode: View {
  @ObservedObject var preset: EQPreset
  @Binding var selectedBandID: UUID?

  var body: some View {
    VStack(spacing: 0) {
      // Preamp section (fixed header)
      HStack {
        Label("Preamp Gain", systemImage: "speaker.wave.2")
          .font(.subheadline)
        Slider(value: $preset.preampGain, in: -20...12, step: 0.1)
        Text(String(format: "%+.1f dB", preset.preampGain))
          .font(.system(.body, design: .monospaced))
          .frame(width: 60, alignment: .trailing)
      }
      .padding()
      .background(Color(nsColor: .controlBackgroundColor))

      Divider()

      List {
        Section("Bands") {
          ForEach(preset.bands) { band in
            BandRow(
              band: band,
              onDelete: {
                if let idx = preset.bands.firstIndex(where: { $0.id == band.id }) {
                  preset.bands.remove(at: idx)
                  if selectedBandID == band.id { selectedBandID = nil }
                }
              }
            )
            .contentShape(Rectangle())
            .onTapGesture { selectedBandID = band.id }
            .listRowBackground(selectedBandID == band.id ? Color.accentColor.opacity(0.1) : nil)
          }

          Button {
            let newBand = EQBand()
            preset.bands.append(newBand)
            selectedBandID = newBand.id
          } label: {
            Label("Add Band", systemImage: "plus.circle")
          }
          .buttonStyle(.borderless)
          .padding(.vertical, 4)
        }
      }
      .listStyle(.inset(alternatesRowBackgrounds: true))
    }
  }
}

private struct BandRow: View {
  @ObservedObject var band: EQBand
  let onDelete: () -> Void

  var body: some View {
    HStack(spacing: 12) {
      // Type Picker
      Picker("", selection: $band.type) {
        ForEach(EQBandType.allCases) { type in
          Text(type.rawValue).tag(type)
        }
      }
      .labelsHidden()
      .controlSize(.small)
      .frame(width: 100)

      // Freq
      HStack(spacing: 4) {
        TextField("", value: $band.freq, format: .number)
          .textFieldStyle(.plain)
          .multilineTextAlignment(.trailing)
          .font(.system(.body, design: .monospaced))
          .frame(width: 60)
        Text("Hz").font(.caption2).foregroundStyle(.secondary)
      }

      // Gain (conditional)
      if band.type.hasGain {
        HStack(spacing: 4) {
          TextField("", value: $band.gain, format: .number)
            .textFieldStyle(.plain)
            .multilineTextAlignment(.trailing)
            .font(.system(.body, design: .monospaced))
            .frame(width: 50)
          Text("dB").font(.caption2).foregroundStyle(.secondary)
        }
      } else {
        Spacer().frame(width: 75)  // Maintain alignment
      }

      // Q (conditional)
      if band.type.hasQ {
        HStack(spacing: 4) {
          TextField("", value: $band.q, format: .number)
            .textFieldStyle(.plain)
            .multilineTextAlignment(.trailing)
            .font(.system(.body, design: .monospaced))
            .frame(width: 50)
          Text("Q").font(.caption2).foregroundStyle(.secondary)
        }
      } else {
        Spacer().frame(width: 70)
      }

      Spacer()

      // Delete Button
      Button(action: onDelete) {
        Image(systemName: "xmark.circle.fill")
          .foregroundStyle(.secondary.opacity(0.5))
          .imageScale(.medium)
      }
      .buttonStyle(.plain)
      .onHover { hovering in
        if hovering { NSCursor.pointingHand.push() } else { NSCursor.pop() }
      }
    }
    .padding(.vertical, 2)
  }
}
