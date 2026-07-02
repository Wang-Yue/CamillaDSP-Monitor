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
    HStack(spacing: 8) {
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

      // Render edit fields based on type:
      if band.type == .free {
        HStack(spacing: 4) {
          coeffField("b0", value: $band.b0)
          coeffField("b1", value: $band.b1)
          coeffField("b2", value: $band.b2)
          coeffField("a1", value: $band.a1)
          coeffField("a2", value: $band.a2)
        }
      } else if band.type == .generalNotch {
        HStack(spacing: 6) {
          paramField("Fc", value: $band.freqNotch, unit: "Hz", width: 55)
          paramField("Fp", value: $band.freqPole, unit: "Hz", width: 55)
          paramField("Qp", value: $band.qPole, unit: "", width: 45)
          Toggle("Norm", isOn: $band.normalizeAtDc)
            .font(.caption2)
            .controlSize(.mini)
            .onChange(of: band.normalizeAtDc) { _, _ in dsp.applyConfig() }
        }
      } else if band.type == .linkwitzTransform {
        HStack(spacing: 6) {
          paramField("Fa", value: $band.freqAct, unit: "Hz", width: 50)
          paramField("Qa", value: $band.qAct, unit: "", width: 45)
          paramField("Ft", value: $band.freqTarget, unit: "Hz", width: 50)
          paramField("Qt", value: $band.qTarget, unit: "", width: 45)
        }
      } else {
        // Standard biquads
        paramField("Fc", value: $band.freq, unit: "Hz", width: 55)

        if band.type.hasGain {
          paramField("Gain", value: $band.gain, unit: "dB", width: 45)
        } else if band.type.isStandard {
          Spacer().frame(width: 65)
        }

        if band.type.hasQ {
          if band.type == .lowshelf || band.type == .highshelf {
            HStack(spacing: 2) {
              TextField("", value: band.useSlope ? $band.slope : $band.q, format: .number)
                .textFieldStyle(.plain)
                .multilineTextAlignment(.trailing)
                .font(.system(.body, design: .monospaced))
                .frame(width: 45)
                .onChange(of: band.q) { _, _ in dsp.applyConfig() }
                .onChange(of: band.slope) { _, _ in dsp.applyConfig() }
              Button(action: {
                band.useSlope.toggle()
                dsp.applyConfig()
              }) {
                Text(band.useSlope ? "dB/o" : "Q")
                  .font(.caption2)
                  .foregroundStyle(Color.accentColor)
                  .underline()
              }
              .buttonStyle(.plain)
            }
          } else if band.type == .notch || band.type == .bandpass || band.type == .allpass {
            HStack(spacing: 2) {
              TextField("", value: band.useBandwidth ? $band.bandwidth : $band.q, format: .number)
                .textFieldStyle(.plain)
                .multilineTextAlignment(.trailing)
                .font(.system(.body, design: .monospaced))
                .frame(width: 45)
                .onChange(of: band.q) { _, _ in dsp.applyConfig() }
                .onChange(of: band.bandwidth) { _, _ in dsp.applyConfig() }
              Button(action: {
                band.useBandwidth.toggle()
                dsp.applyConfig()
              }) {
                Text(band.useBandwidth ? "oct" : "Q")
                  .font(.caption2)
                  .foregroundStyle(Color.accentColor)
                  .underline()
              }
              .buttonStyle(.plain)
            }
          } else {
            paramField("Q", value: $band.q, unit: "", width: 45)
          }
        } else if band.type.isStandard {
          Spacer().frame(width: 65)
        }
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

  @ViewBuilder
  private func coeffField(_ label: String, value: Binding<Double>) -> some View {
    HStack(spacing: 2) {
      Text(label).font(.caption2).foregroundStyle(.secondary)
      TextField("", value: value, format: .number)
        .textFieldStyle(.plain)
        .multilineTextAlignment(.trailing)
        .font(.system(.body, design: .monospaced))
        .frame(width: 45)
        .onChange(of: value.wrappedValue) { _, _ in dsp.applyConfig() }
    }
  }

  @ViewBuilder
  private func paramField(_ label: String, value: Binding<Double>, unit: String, width: CGFloat)
    -> some View
  {
    HStack(spacing: 2) {
      TextField("", value: value, format: .number)
        .textFieldStyle(.plain)
        .multilineTextAlignment(.trailing)
        .font(.system(.body, design: .monospaced))
        .frame(width: width)
        .onChange(of: value.wrappedValue) { _, _ in dsp.applyConfig() }
      if !unit.isEmpty {
        Text(unit).font(.caption2).foregroundStyle(.secondary)
      } else {
        Text(label).font(.caption2).foregroundStyle(.secondary)
      }
    }
  }
}
