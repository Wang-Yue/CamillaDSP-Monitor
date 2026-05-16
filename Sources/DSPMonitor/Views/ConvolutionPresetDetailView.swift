// ConvolutionPresetDetailView — view & rename a saved IR preset.
//
// Mirrors `EQPresetDetailView`'s shape: an editable name field, an
// inline IR plot, and a metadata strip. Multi-rate presets get a
// rate picker so the user can preview each member of the family.
// The on-disk file paths are exposed in a list with Reveal-in-Finder
// affordances.

import AppKit
import Observation
import SwiftUI

struct ConvolutionPresetDetailView: View {
  @Bindable var preset: ConvolutionPreset
  @Environment(PipelineStore.self) var pipeline
  @Environment(AudioDeviceManager.self) var devices
  /// Sample rate currently selected for previewing. Defaults to the
  /// engine's live capture rate (or the closest available).
  @State private var previewRate: Int = 48_000

  var body: some View {
    VStack(spacing: 0) {
      HStack {
        Image(systemName: "waveform.badge.magnifyingglass")
          .font(.title2)
          .foregroundStyle(Color.accentColor)
        TextField("Preset Name", text: $preset.name)
          .font(.title2.bold())
          .textFieldStyle(.roundedBorder)
          .frame(maxWidth: 300)
          .onSubmit { NSApp.keyWindow?.makeFirstResponder(nil) }
        Spacer()
        Button(role: .destructive) {
          deletePreset()
        } label: {
          Label("Delete", systemImage: "trash")
        }
      }
      .padding()

      Divider()

      ScrollView {
        VStack(alignment: .leading, spacing: 16) {
          metadataCard
          irPlotCard
          fileListCard
        }
        .padding()
      }
    }
    .background(Color(nsColor: .controlBackgroundColor))
    .onAppear { syncPreviewRate() }
    .onChange(of: preset.name) { _, _ in pipeline.updateConvPreset() }
  }

  private var metadataCard: some View {
    GroupBox("Details") {
      VStack(alignment: .leading, spacing: 6) {
        row("Kind", preset.kindLabel)
        row("Taps", "\(preset.taps)")
        let rates = preset.availableSampleRates
        row("Rates", rates.isEmpty ? "—" : rates.map { "\($0 / 1000)k" }.joined(separator: " / "))
        let ms = preset.latencyMilliseconds(atSampleRate: previewRate)
        row(
          "Latency @ \(previewRate / 1000)k",
          ms > 0 ? String(format: "%.1f ms", ms) : "≈ 0 ms (min-phase)")
      }
      .padding(.vertical, 4)
      .frame(maxWidth: .infinity, alignment: .leading)
    }
  }

  @ViewBuilder
  private var irPlotCard: some View {
    GroupBox("Impulse Response") {
      VStack(alignment: .leading, spacing: 8) {
        let rates = preset.availableSampleRates
        if rates.count > 1 {
          HStack {
            Text("Preview rate")
              .font(.caption)
              .foregroundStyle(.secondary)
            Picker("", selection: $previewRate) {
              ForEach(rates, id: \.self) { r in
                Text("\(r) Hz").tag(r)
              }
            }
            .labelsHidden()
            .frame(maxWidth: 160)
            Spacer()
          }
        }
        if let path = preset.irPath(forSampleRate: previewRate) {
          ConvolutionIRPlot(irPath: path)
            .frame(height: 180)
        } else {
          Text("No IR available for \(previewRate) Hz.")
            .font(.callout)
            .foregroundStyle(.secondary)
            .padding(8)
        }
      }
      .padding(.vertical, 4)
    }
  }

  private var fileListCard: some View {
    GroupBox("Files") {
      VStack(alignment: .leading, spacing: 6) {
        ForEach(preset.availableSampleRates, id: \.self) { rate in
          if let path = preset.irPaths[rate] {
            HStack(alignment: .top) {
              Text("\(rate) Hz")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(width: 80, alignment: .leading)
              Text(path)
                .font(.system(.caption, design: .monospaced))
                .lineLimit(1)
                .truncationMode(.middle)
              Spacer()
              Button {
                NSWorkspace.shared.activateFileViewerSelecting(
                  [URL(fileURLWithPath: path)])
              } label: {
                Image(systemName: "folder")
              }
              .buttonStyle(.plain)
              .foregroundStyle(.secondary)
            }
          }
        }
      }
      .padding(.vertical, 4)
      .frame(maxWidth: .infinity, alignment: .leading)
    }
  }

  private func row(_ label: String, _ value: String) -> some View {
    HStack {
      Text(label)
        .foregroundStyle(.secondary)
        .frame(width: 130, alignment: .leading)
      Text(value)
        .font(.system(.body, design: .monospaced))
      Spacer()
    }
  }

  private func deletePreset() {
    if let idx = pipeline.convPresets.firstIndex(where: { $0.id == preset.id }) {
      pipeline.deleteConvPreset(at: idx)
    }
  }

  /// Default `previewRate` to the engine's live capture rate, falling
  /// back to the closest available IR.
  private func syncPreviewRate() {
    let liveRate = devices.captureConfig.sampleRate
    let available = preset.availableSampleRates
    if available.contains(liveRate) {
      previewRate = liveRate
    } else if !available.isEmpty {
      // Closest by log-distance (matches preset.irPath fallback).
      let target = log(Double(liveRate))
      previewRate =
        available.min(by: {
          abs(log(Double($0)) - target) < abs(log(Double($1)) - target)
        }) ?? available[0]
    }
  }
}
