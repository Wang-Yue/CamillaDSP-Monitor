import Observation
import SwiftUI

struct OratoryPresetPickerView: View {
  @Environment(PipelineStore.self) var pipeline
  @Environment(\.dismiss) var dismiss

  @State private var headphones: [OratoryHeadphone] = []
  @State private var searchText = ""
  @State private var isLoading = true
  @State private var isImporting = false
  @State private var errorMessage: String?

  var filteredHeadphones: [OratoryHeadphone] {
    if searchText.isEmpty {
      return Array(headphones.prefix(50))
    } else {
      return headphones.filter {
        $0.name.localizedCaseInsensitiveContains(searchText)
      }
    }
  }

  var body: some View {
    NavigationStack {
      VStack(spacing: 0) {
        HStack(spacing: 16) {
          VStack(alignment: .leading) {
            Text("Oratory1990 Database")
              .font(.headline)
            Text("Hand-measured presets based on Oratory1990 targets")
              .font(.caption)
              .foregroundStyle(.secondary)
          }
          .fixedSize()

          TextField("Search \(headphones.count) headphones...", text: $searchText)
            .textFieldStyle(.roundedBorder)
            .frame(maxWidth: .infinity)
        }
        .padding()

        Divider()

        List(filteredHeadphones) { headphone in
          Button {
            importHeadphone(headphone)
          } label: {
            VStack(alignment: .leading) {
              Text(headphone.name)
                .font(.headline)
              Text(headphone.path)
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .truncationMode(.head)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.vertical, 4)
            .padding(.horizontal, 8)
            .contentShape(Rectangle())
          }
          .buttonStyle(.plain)
          .disabled(isImporting)
        }
        .listStyle(.plain)
        .overlay {
          if isLoading {
            ProgressView("Loading Oratory1990 database...")
          } else if isImporting {
            ZStack {
              Color(nsColor: .windowBackgroundColor).opacity(0.5)
              ProgressView("Importing EQ profile...")
            }
          } else if let error = errorMessage {
            ContentUnavailableView(
              "Error", systemImage: "exclamationmark.triangle", description: Text(error))
          } else if filteredHeadphones.isEmpty && !isLoading {
            ContentUnavailableView.search
          }
        }
      }
      .toolbar {
        ToolbarItem(placement: .cancellationAction) {
          Button("Cancel") { dismiss() }
        }
        ToolbarItem(placement: .primaryAction) {
          Button {
            refreshDatabase()
          } label: {
            Label("Refresh", systemImage: "arrow.clockwise")
          }
          .disabled(isLoading || isImporting)
          .help("Force refresh database from GitHub")
        }
      }
    }
    .task { await loadDatabase() }
    .frame(width: 500, height: 600)
  }

  private func loadDatabase() async {
    isLoading = true
    errorMessage = nil
    do {
      headphones = try await OratoryPresetService.shared.fetchAllHeadphones()
      isLoading = false
    } catch {
      errorMessage = "Failed to fetch database: \(error.localizedDescription)"
      isLoading = false
    }
  }

  private func refreshDatabase() {
    Task {
      isLoading = true
      errorMessage = nil
      do {
        headphones = try await OratoryPresetService.shared.fetchAllHeadphones(forceRefresh: true)
        isLoading = false
      } catch {
        errorMessage = "Failed to refresh database: \(error.localizedDescription)"
        isLoading = false
      }
    }
  }

  private func importHeadphone(_ headphone: OratoryHeadphone) {
    guard !isImporting else { return }
    isImporting = true
    errorMessage = nil

    Task {
      do {
        let text = try await OratoryPresetService.shared.fetchParametricEQ(for: headphone)
        if let result = EQPreset.fromCSV(text) {
          let presetName = "[Oratory] \(headphone.name)"
          pipeline.addEQPreset(name: presetName, preamp: result.preamp, bands: result.bands)
          dismiss()
        } else {
          errorMessage = "Could not parse EQ data. File format might have changed."
          isImporting = false
        }
      } catch {
        errorMessage = "Network error: \(error.localizedDescription)"
        isImporting = false
      }
    }
  }
}
