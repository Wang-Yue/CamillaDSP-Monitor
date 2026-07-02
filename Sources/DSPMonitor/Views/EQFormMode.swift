// EQFormMode - Simplified list view for editing EQ bands

import DSPConfig
import Observation
import SwiftUI

struct EQFormMode: View {
  @Bindable var preset: EQPreset
  @Binding var selectedBandID: UUID?
  @Environment(DSPEngineController.self) var dsp

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
            VStack(alignment: .leading, spacing: 8) {
              BandRow(
                band: band,
                onDelete: {
                  if let idx = preset.bands.firstIndex(where: { $0.id == band.id }) {
                    preset.bands.remove(at: idx)
                    if selectedBandID == band.id { selectedBandID = nil }
                    dsp.applyConfig()
                  }
                }
              )
              .contentShape(Rectangle())
              .onTapGesture { selectedBandID = band.id }

              if !band.type.isStandard {
                Group {
                  switch band.type {
                  case .free:
                    FreeBiquadFields(band: band)
                  case .generalNotch:
                    GeneralNotchFields(band: band)
                  case .linkwitzTransform:
                    LinkwitzTransformFields(band: band)
                  default:
                    EmptyView()
                  }
                }
                .padding(.leading, 28)
                .padding(.bottom, 8)
              }
            }
            .listRowBackground(selectedBandID == band.id ? Color.accentColor.opacity(0.05) : nil)
          }

          Button {
            let newBand = EQBand()
            preset.bands.append(newBand)
            selectedBandID = newBand.id
            dsp.applyConfig()
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
  @Bindable var band: EQBand
  let onDelete: () -> Void
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    HStack(spacing: 12) {
      // Enabled toggle
      Toggle("", isOn: $band.isEnabled)
        .labelsHidden()
        .toggleStyle(.checkbox)
        .onChange(of: band.isEnabled) { _, _ in
          dsp.applyConfig()
        }

      // Type Picker
      Picker("", selection: $band.type) {
        ForEach(EQBandType.allCases) { type in
          Text(type.rawValue).tag(type)
        }
      }
      .labelsHidden()
      .controlSize(.small)
      .frame(width: 130)
      .onChange(of: band.type) { _, _ in
        dsp.applyConfig()
      }

      if band.type.isStandard {
        // Freq
        HStack(spacing: 4) {
          TextField("", value: $band.freq, format: .number)
            .textFieldStyle(.plain)
            .multilineTextAlignment(.trailing)
            .font(.system(.body, design: .monospaced))
            .frame(width: 60)
            .onChange(of: band.freq) { _, _ in
              dsp.applyConfig()
            }
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
              .onChange(of: band.gain) { _, _ in
                dsp.applyConfig()
              }
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
              .onChange(of: band.q) { _, _ in
                dsp.applyConfig()
              }
            Text("Q").font(.caption2).foregroundStyle(.secondary)
          }
        } else {
          Spacer().frame(width: 70)
        }
      } else {
        Text("Configure parameters below")
          .font(.caption)
          .foregroundStyle(.secondary)
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

// MARK: - Advanced Fields

struct FreeBiquadFields: View {
  @Bindable var band: EQBand
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      Text("Coefficients (Direct Form I)").font(.caption.bold()).foregroundStyle(.secondary)
      Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 6) {
        GridRow {
          Text("b0").font(.caption).foregroundStyle(.secondary)
          TextField("b0", value: $band.b0, format: .number).textFieldStyle(.roundedBorder).frame(
            width: 80
          ).onSubmit { dsp.applyConfig() }
          Text("a1").font(.caption).foregroundStyle(.secondary)
          TextField("a1", value: $band.a1, format: .number).textFieldStyle(.roundedBorder).frame(
            width: 80
          ).onSubmit { dsp.applyConfig() }
        }
        GridRow {
          Text("b1").font(.caption).foregroundStyle(.secondary)
          TextField("b1", value: $band.b1, format: .number).textFieldStyle(.roundedBorder).frame(
            width: 80
          ).onSubmit { dsp.applyConfig() }
          Text("a2").font(.caption).foregroundStyle(.secondary)
          TextField("a2", value: $band.a2, format: .number).textFieldStyle(.roundedBorder).frame(
            width: 80
          ).onSubmit { dsp.applyConfig() }
        }
        GridRow {
          Text("b2").font(.caption).foregroundStyle(.secondary)
          TextField("b2", value: $band.b2, format: .number).textFieldStyle(.roundedBorder).frame(
            width: 80
          ).onSubmit { dsp.applyConfig() }
          Text("")
          Text("")
        }
      }
    }
  }
}

struct GeneralNotchFields: View {
  @Bindable var band: EQBand
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack(spacing: 12) {
        Text("Notch Freq").font(.caption).foregroundStyle(.secondary).frame(
          width: 80, alignment: .leading)
        Slider(value: $band.freqNotch, in: 20...20000, step: 1).onChange(of: band.freqNotch) {
          _, _ in dsp.applyConfig()
        }
        Text("\(Int(band.freqNotch)) Hz").font(.system(.caption, design: .monospaced)).frame(
          width: 80, alignment: .trailing)
      }
      HStack(spacing: 12) {
        Text("Pole Freq").font(.caption).foregroundStyle(.secondary).frame(
          width: 80, alignment: .leading)
        Slider(value: $band.freqPole, in: 20...20000, step: 1).onChange(of: band.freqPole) { _, _ in
          dsp.applyConfig()
        }
        Text("\(Int(band.freqPole)) Hz").font(.system(.caption, design: .monospaced)).frame(
          width: 80, alignment: .trailing)
      }
      Toggle("Normalize at DC", isOn: $band.normalizeAtDc)
        .font(.caption)
        .onChange(of: band.normalizeAtDc) { _, _ in dsp.applyConfig() }
    }
    .frame(maxWidth: 450)
  }
}

struct LinkwitzTransformFields: View {
  @Bindable var band: EQBand
  @Environment(DSPEngineController.self) var dsp

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack(spacing: 12) {
        Text("F(act)").font(.caption).foregroundStyle(.secondary).frame(
          width: 80, alignment: .leading)
        Slider(value: $band.freqAct, in: 1...200, step: 0.5).onChange(of: band.freqAct) { _, _ in
          dsp.applyConfig()
        }
        Text(String(format: "%.1f Hz", band.freqAct)).font(.system(.caption, design: .monospaced))
          .frame(width: 80, alignment: .trailing)
      }
      HStack(spacing: 12) {
        Text("Q(act)").font(.caption).foregroundStyle(.secondary).frame(
          width: 80, alignment: .leading)
        Slider(value: $band.qAct, in: 0.1...3.0, step: 0.01).onChange(of: band.qAct) { _, _ in
          dsp.applyConfig()
        }
        Text(String(format: "%.3f", band.qAct)).font(.system(.caption, design: .monospaced)).frame(
          width: 80, alignment: .trailing)
      }
      HStack(spacing: 12) {
        Text("F(target)").font(.caption).foregroundStyle(.secondary).frame(
          width: 80, alignment: .leading)
        Slider(value: $band.freqTarget, in: 1...200, step: 0.5).onChange(of: band.freqTarget) {
          _, _ in dsp.applyConfig()
        }
        Text(String(format: "%.1f Hz", band.freqTarget)).font(
          .system(.caption, design: .monospaced)
        ).frame(width: 80, alignment: .trailing)
      }
      HStack(spacing: 12) {
        Text("Q(target)").font(.caption).foregroundStyle(.secondary).frame(
          width: 80, alignment: .leading)
        Slider(value: $band.qTarget, in: 0.1...3.0, step: 0.01).onChange(of: band.qTarget) { _, _ in
          dsp.applyConfig()
        }
        Text(String(format: "%.3f", band.qTarget)).font(.system(.caption, design: .monospaced))
          .frame(width: 80, alignment: .trailing)
      }
    }
    .frame(maxWidth: 450)
  }
}
