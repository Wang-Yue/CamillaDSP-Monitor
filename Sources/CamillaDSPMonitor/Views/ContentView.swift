// ContentView - Main app layout with sidebar navigation and detail panel

import SwiftUI

struct ContentView: View {
  @EnvironmentObject var appState: AppState
  @State private var selection: SidebarItem? = .devices
  @State private var showAutoEqSearch = false

  var body: some View {
    // When the mini player is active the main window is hidden. Rendering nothing
    // here removes all level/spectrum subscriptions from the hidden window, stopping
    // SwiftUI from evaluating view bodies and running Canvas closures at 10 Hz for
    // content the user cannot see.
    if appState.isMiniPlayerActive {
      Color.clear
    } else {
      NavigationSplitView {
        SidebarView(selection: $selection, showAutoEqSearch: $showAutoEqSearch)
          .toolbar {
            ToolbarItem(placement: .primaryAction) {
              Button {
                MiniPlayerWindowController.shared.showMiniPlayer(appState: appState)
              } label: {
                Image(systemName: "pip")
                  .imageScale(.large)
                  .fontWeight(.medium)
                  .contentShape(Rectangle())
              }
              .buttonStyle(.borderless)
              .help("Mini Player")
            }
          }
      } detail: {
        DetailPanel(selection: selection)
          .environmentObject(appState)
      }
      .toolbar {
        ToolbarView()
      }
      .navigationTitle("CamillaDSP Monitor")
      .sheet(isPresented: $showAutoEqSearch) {
        AutoEqPickerView()
          .environmentObject(appState)
      }
    }
  }
}

// MARK: - Sidebar Item

enum SidebarItem: Hashable {
  case devices
  case levels
  case spectrum
  case logs
  case dashboard
  case resampler
  case stage(Int)
  case eqPreset(Int)
}

// MARK: - Toolbar

struct ToolbarView: ToolbarContent {
  @EnvironmentObject var appState: AppState

  var body: some ToolbarContent {
    ToolbarItemGroup(placement: .primaryAction) {

      Group {
        switch appState.status {
        case .starting, .applyingConfig:
          ProgressView()
            .controlSize(.small)
            .padding(.trailing, 4)
        case .running:
          Button {
            appState.stopEngine()
          } label: {
            Label("Stop", systemImage: "stop.circle.fill")
              .foregroundStyle(.red)
          }
          .help("Stop Engine")
        case .inactive:
          Button {
            appState.startEngine()
          } label: {
            Label("Start", systemImage: "play.circle.fill")
              .foregroundStyle(.green)
          }
          .help("Start Engine")
        case .error(let msg):
          Button {
            appState.startEngine()
          } label: {
            Label("Error", systemImage: "exclamationmark.circle.fill")
              .foregroundStyle(.orange)
          }
          .help(msg)
        }
      }

      Text("\(appState.sampleRate) Hz")
        .font(.system(.body, design: .monospaced))
        .foregroundStyle(.secondary)

      // Observed wrapper to ensure updates
      CPUUsageView(load: appState.load)

      VolumeControlView()
    }
  }
}

struct CPUUsageView: View {
  @ObservedObject var load: LoadState
  var body: some View {
    Text(String(format: "%.0f%%", load.processingLoad))
      .font(.system(.caption, design: .monospaced))
      .foregroundStyle(load.processingLoad > 80 ? .red : .secondary)
  }
}

// MARK: - Sidebar

struct SidebarView: View {
  @EnvironmentObject var appState: AppState
  @Binding var selection: SidebarItem?
  @Binding var showAutoEqSearch: Bool

  var body: some View {
    List(selection: $selection) {
      Section("Audio") {
        Label("Devices", systemImage: "hifispeaker.2")
          .tag(SidebarItem.devices)
        Label("Dashboard", systemImage: "gauge.open.with.lines.needle.33percent")
          .tag(SidebarItem.dashboard)
      }

      Section("Monitoring") {
        Label("Level Meters", systemImage: "chart.bar")
          .tag(SidebarItem.levels)
        Label("Spectrum", systemImage: "waveform.path.ecg.rectangle")
          .tag(SidebarItem.spectrum)
        Label("Console Logs", systemImage: "terminal")
          .tag(SidebarItem.logs)
      }

      Section("Pipeline") {
        ResamplerSidebarRow()
          .tag(SidebarItem.resampler)

        ForEach(appState.stages.indices, id: \.self) { index in
          PipelineSidebarRow(stage: appState.stages[index])
            .tag(SidebarItem.stage(index))
        }
      }

      Section("EQ Presets") {
        ForEach(appState.eqPresets.indices, id: \.self) { index in
          EQPresetSidebarRow(preset: appState.eqPresets[index])
            .tag(SidebarItem.eqPreset(index))
            .contextMenu {
              Button(role: .destructive) {
                appState.deleteEQPreset(at: index)
              } label: {
                Label("Delete", systemImage: "trash")
              }
            }
        }

        HStack {
          Button {
            appState.addEQPreset()
          } label: {
            Label("Add", systemImage: "plus")
              .foregroundStyle(.secondary)
          }

          Spacer()

          Button {
            showAutoEqSearch = true
          } label: {
            Label("AutoEQ", systemImage: "magnifyingglass")
              .foregroundStyle(.secondary)
          }
        }
        .buttonStyle(.plain)
        .font(.caption)
      }
    }
    .listStyle(.sidebar)
    .navigationSplitViewColumnWidth(min: 220, ideal: 260)
  }
}

// MARK: - Detail Panel (routes based on sidebar selection)

struct DetailPanel: View {
  let selection: SidebarItem?
  @EnvironmentObject var appState: AppState

  var body: some View {
    VStack(spacing: 0) {
      // Always-visible compact level bar
      CompactLevelMeterBar()
        .padding(.horizontal)
        .padding(.vertical, 8)

      Divider()

      // Routed content
      switch selection {
      case .devices:
        DevicePickerView()
      case .dashboard, .none:
        DashboardView()
      case .levels:
        LevelMetersCard()
          .padding()
          .frame(maxHeight: .infinity, alignment: .top)
          .background(Color(nsColor: .controlBackgroundColor))
      case .spectrum:
        SpectrumCard()
          .padding()
          .frame(maxHeight: .infinity, alignment: .top)
          .background(Color(nsColor: .controlBackgroundColor))
      case .logs:
        ConsoleLogsView()
          .environmentObject(appState.logManager)
      case .resampler:
        ResamplerDetailView()
          .padding()
          .frame(maxHeight: .infinity, alignment: .top)
          .background(Color(nsColor: .controlBackgroundColor))
      case .eqPreset(let index):
        if index < appState.eqPresets.count {
          EQPresetDetailView(preset: appState.eqPresets[index])
        }
      case .stage(let index):
        StageDetailView(stageIndex: index)
      }
    }
  }
}

// MARK: - Pipeline Sidebar Row

struct ResamplerSidebarRow: View {
  @EnvironmentObject var appState: AppState

  var body: some View {
    HStack {
      Image(systemName: "arrow.triangle.2.circlepath")
        .frame(width: 20)
        .foregroundStyle(appState.resamplerEnabled ? Color.accentColor : Color.secondary)
      Text("Resampler")
        .foregroundStyle(appState.resamplerEnabled ? .primary : .secondary)
      Spacer()
      Toggle("", isOn: $appState.resamplerEnabled)
        .labelsHidden()
        .toggleStyle(.switch)
        .controlSize(.mini)
    }
  }
}

struct EQPresetSidebarRow: View {
  @ObservedObject var preset: EQPreset

  var body: some View {
    Label(preset.name, systemImage: "slider.horizontal.3")
  }
}

struct PipelineSidebarRow: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var appState: AppState

  var body: some View {
    HStack {
      Image(systemName: stage.type.icon)
        .frame(width: 20)
        .foregroundStyle(stage.isEnabled ? Color.accentColor : Color.secondary)
      Text(stage.name)
        .foregroundStyle(stage.isEnabled ? .primary : .secondary)
      Spacer()
      Toggle("", isOn: $stage.isEnabled)
        .labelsHidden()
        .toggleStyle(.switch)
        .controlSize(.mini)
        .onChange(of: stage.isEnabled) { _, _ in
          appState.applyConfig()
        }
    }
  }
}
