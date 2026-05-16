import AppKit
import SwiftUI

struct ConvolutionImportView: View {
  @Environment(PipelineStore.self) var pipeline
  @Environment(\.dismiss) var dismiss

  @State private var presetName: String = ""
  @State private var kindLabel: String = "Imported"
  @State private var items: [ConvolutionImportService.ImportItem] = []
  @State private var errorMessage: String?
  @State private var isImporting: Bool = false

  private var standardRates: [Int] {
    ConvolutionImportService.standardRates
  }

  var isImportDisabled: Bool {
    items.isEmpty || presetName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
      || hasDuplicateRates
  }

  var hasDuplicateRates: Bool {
    let rates = items.map { $0.sampleRate }
    return Set(rates).count != rates.count
  }

  var body: some View {
    NavigationStack {
      VStack(spacing: 0) {
        // Header Block
        VStack(alignment: .leading, spacing: 4) {
          Text("Import Impulse Responses")
            .font(.headline)
          Text("Import files as a unified multi-rate Convolution Preset.")
            .font(.caption)
            .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding()
        .background(.ultraThinMaterial)

        Divider()

        ScrollView {
          VStack(spacing: 20) {
            // Config Group Box
            GroupBox("Preset Details") {
              VStack(spacing: 12) {
                HStack {
                  Text("Preset Name")
                    .frame(width: 100, alignment: .leading)
                  TextField("e.g., My Custom IR", text: $presetName)
                    .textFieldStyle(.roundedBorder)
                }

                HStack {
                  Text("Kind Label")
                    .frame(width: 100, alignment: .leading)
                  TextField("e.g., Imported, Min-phase", text: $kindLabel)
                    .textFieldStyle(.roundedBorder)
                }
              }
              .padding(.vertical, 4)
            }

            // File List
            VStack(alignment: .leading, spacing: 8) {
              HStack {
                Text("Impulse Response Files")
                  .font(.subheadline.bold())
                Spacer()
                Button {
                  selectFiles()
                } label: {
                  Label("Add File(s)…", systemImage: "plus")
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
              }

              if items.isEmpty {
                VStack(spacing: 12) {
                  Image(systemName: "arrow.down.doc")
                    .font(.largeTitle)
                    .foregroundStyle(.tertiary)
                  Text("No files selected. Click 'Add File(s)' to begin.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 40)
                .background(
                  RoundedRectangle(cornerRadius: 8).stroke(
                    .separator, style: StrokeStyle(dash: [4])))
              } else {
                VStack(spacing: 12) {
                  ForEach($items) { $item in
                    VStack(alignment: .leading, spacing: 8) {
                      HStack {
                        Image(systemName: item.format == "WAV" ? "waveform.circle" : "doc.text")
                          .foregroundStyle(Color.accentColor)
                        Text(item.fileURL.lastPathComponent)
                          .font(.caption.bold())
                          .lineLimit(1)
                          .truncationMode(.middle)
                        Spacer()
                        Button(role: .destructive) {
                          items.removeAll { $0.id == item.id }
                        } label: {
                          Image(systemName: "trash")
                            .foregroundStyle(.red)
                        }
                        .buttonStyle(.plain)
                      }

                      Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                        GridRow {
                          Text("Sample Rate")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                          Picker("", selection: $item.sampleRate) {
                            ForEach(standardRates, id: \.self) { rate in
                              Text("\(rate) Hz").tag(rate)
                            }
                          }
                          .labelsHidden()
                          .controlSize(.small)
                          .frame(width: 140)
                        }

                        GridRow {
                          Text("Format")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                          Picker("", selection: $item.format) {
                            ForEach(ConvolutionImportService.formats, id: \.self) { fmt in
                              Text(fmt).tag(fmt)
                            }
                          }
                          .labelsHidden()
                          .controlSize(.small)
                          .frame(width: 140)
                        }

                        if item.format == "WAV" {
                          GridRow {
                            Text("WAV Channel")
                              .font(.caption)
                              .foregroundStyle(.secondary)
                            HStack {
                              Stepper("Channel \(item.channel)", value: $item.channel, in: 0...15)
                                .font(.caption)
                            }
                            .frame(width: 140)
                          }
                        }
                      }
                    }
                    .padding()
                    .background(
                      RoundedRectangle(cornerRadius: 8).fill(
                        Color(nsColor: .controlBackgroundColor))
                    )
                    .overlay(
                      RoundedRectangle(cornerRadius: 8)
                        .stroke(.separator, lineWidth: 1)
                    )
                  }

                  if hasDuplicateRates {
                    HStack {
                      Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                      Text(
                        "Duplicate sample rates found. Each file in the preset must represent a different sample rate."
                      )
                      .font(.caption)
                      .foregroundStyle(.secondary)
                    }
                    .padding(.top, 4)
                  }
                }
              }
            }

            if let error = errorMessage {
              HStack {
                Image(systemName: "exclamationmark.octagon.fill")
                  .foregroundStyle(.red)
                Text(error)
                  .font(.callout)
                  .foregroundStyle(.red)
              }
              .padding()
              .frame(maxWidth: .infinity, alignment: .leading)
              .background(RoundedRectangle(cornerRadius: 8).fill(Color.red.opacity(0.1)))
            }
          }
          .padding()
        }

        Divider()

        // Footer Buttons
        HStack(spacing: 12) {
          Spacer()
          Button("Cancel") {
            dismiss()
          }
          .keyboardShortcut(.cancelAction)

          Button {
            executeImport()
          } label: {
            if isImporting {
              ProgressView()
                .controlSize(.small)
                .padding(.horizontal, 10)
            } else {
              Text("Import")
            }
          }
          .buttonStyle(.borderedProminent)
          .disabled(isImportDisabled || isImporting)
          .keyboardShortcut(.defaultAction)
        }
        .padding()
        .background(.ultraThinMaterial)
      }
    }
    .frame(width: 480, height: 580)
  }

  private func selectFiles() {
    let panel = NSOpenPanel()
    panel.allowsMultipleSelection = true
    panel.canChooseDirectories = false
    panel.allowedContentTypes = [.wav, .data, .item]

    if panel.runModal() == .OK {
      for url in panel.urls {
        let meta = ConvolutionImportService.shared.inferMetadata(for: url)
        let item = ConvolutionImportService.ImportItem(
          fileURL: url,
          sampleRate: meta.sampleRate,
          format: meta.format
        )
        items.append(item)
      }

      // Autofill preset name if empty and we imported at least one file
      if presetName.isEmpty, let first = items.first {
        let base = first.fileURL.deletingPathExtension().lastPathComponent
        // Strip potential sample rate tags from base name to make it cleaner
        let cleaned = base.replacingOccurrences(
          of: "-\\d+Hz", with: "", options: .regularExpression
        )
        .replacingOccurrences(of: "_\\d+", with: "", options: .regularExpression)
        presetName = cleaned
      }
    }
  }

  private func executeImport() {
    isImporting = true
    errorMessage = nil

    Task {
      do {
        _ = try ConvolutionImportService.shared.importPreset(
          name: presetName,
          kindLabel: kindLabel,
          items: items,
          pipeline: pipeline
        )
        dismiss()
      } catch {
        errorMessage = error.localizedDescription
        isImporting = false
      }
    }
  }
}
