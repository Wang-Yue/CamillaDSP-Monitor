import SwiftUI

struct AutoEqPickerView: View {
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) var dismiss
    
    @State private var headphones: [AutoEqHeadphone] = []
    @State private var searchText = ""
    @State private var isLoading = true
    @State private var isImporting = false
    @State private var errorMessage: String?
    
    var filteredHeadphones: [AutoEqHeadphone] {
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
                }
                .disabled(isImporting)
            }
            .navigationTitle("AutoEQ Database")
            .searchable(text: $searchText, prompt: "Search \(headphones.count) headphones...")
            .overlay {
                if isLoading {
                    ProgressView("Loading database...")
                } else if isImporting {
                    ZStack {
                        Color(nsColor: .windowBackgroundColor).opacity(0.5)
                        ProgressView("Importing EQ profile...")
                    }
                } else if let error = errorMessage {
                    ContentUnavailableView("Error", systemImage: "exclamationmark.triangle", description: Text(error))
                } else if filteredHeadphones.isEmpty && !isLoading {
                    ContentUnavailableView.search
                }
            }
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        dismiss()
                    }
                }
            }
        }
        .task {
            do {
                headphones = try await AutoEqService.shared.fetchAllHeadphones()
                isLoading = false
            } catch {
                errorMessage = "Failed to fetch database: \(error.localizedDescription)"
                isLoading = false
            }
        }
        .frame(width: 500, height: 600)
    }
    
    private func importHeadphone(_ headphone: AutoEqHeadphone) {
        guard !isImporting else { return }
        isImporting = true
        errorMessage = nil
        
        Task {
            do {
                let text = try await AutoEqService.shared.fetchParametricEQ(for: headphone)
                if let result = EQPreset.fromCSV(text) {
                    appState.addEQPreset(name: headphone.name, preamp: result.preamp, bands: result.bands)
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
