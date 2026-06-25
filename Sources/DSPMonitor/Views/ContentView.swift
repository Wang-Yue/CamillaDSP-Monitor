// ContentView - Main app layout with sidebar navigation and detail panel

import Observation
import SwiftUI

struct ContentView: View {
  @Environment(AppState.self) var appState
  @State private var selection: SidebarItem? = .devices
  @State private var showAutoEqSearch = false
  @State private var showOratorySearch = false

  var body: some View {
    NavigationSplitView {
      SidebarView(
        selection: $selection, showAutoEqSearch: $showAutoEqSearch,
        showOratorySearch: $showOratorySearch)
    } detail: {
      DetailPanel(selection: selection)
    }
    .toolbar {
      ToolbarView()
    }
    .navigationTitle("DSP Monitor")
    .sheet(isPresented: $showAutoEqSearch) {
      AutoEqPickerView()
        .environment(appState.pipeline)
    }
    .sheet(isPresented: $showOratorySearch) {
      OratoryPresetPickerView()
        .environment(appState.pipeline)
    }
  }
}

// MARK: - Sidebar Item

enum SidebarItem: Hashable {
  case devices
  case logs
  case dashboard
  case resampler
  case stage(Int)
  /// Identified by UUID rather than array index so deleting a
  /// preset doesn't leave stale indices in `ForEach` closures
  /// (SwiftUI keeps the closures around briefly during the
  /// transition; an Int index that was valid before deletion
  /// becomes out-of-range after).
  case eqPreset(UUID)
}

// MARK: - Toolbar

struct ToolbarView: ToolbarContent {
  @Environment(DSPEngineController.self) var dsp
  @Environment(AudioDeviceManager.self) var devices

  var body: some ToolbarContent {
    ToolbarItemGroup(placement: .primaryAction) {
      Group {
        switch dsp.status {
        case .starting:
          ProgressView()
            .controlSize(.small)
            .padding(.trailing, 4)
        case .running, .paused, .stalled:
          Button {
            dsp.stopEngine()
          } label: {
            Label("Stop", systemImage: "stop.circle.fill")
              .foregroundStyle(.red)
          }
          .help("Stop Engine")
        case .inactive:
          Button {
            dsp.startEngine()
          } label: {
            Label("Start", systemImage: "play.circle.fill")
              .foregroundStyle(.green)
          }
          .help("Start Engine")
        }
      }

      Text("\(devices.captureConfig.sampleRate) Hz")
        .font(.system(.body, design: .monospaced))
        .foregroundStyle(.secondary)
        .padding(.trailing, 8)

      VolumeControlView()
    }
  }
}

// MARK: - Sidebar

struct SidebarView: View {
  @Environment(AudioSettings.self) var settings
  @Environment(PipelineStore.self) var pipeline
  @Environment(AppState.self) var appState
  @Binding var selection: SidebarItem?
  @Binding var showAutoEqSearch: Bool
  @Binding var showOratorySearch: Bool

  var body: some View {
    List(selection: $selection) {
      Section("Audio") {
        Label("Devices", systemImage: "hifispeaker.2")
          .tag(SidebarItem.devices)

        Label("Dashboard", systemImage: "gauge.open.with.lines.needle.33percent")
          .tag(SidebarItem.dashboard)

        Label("Console Logs", systemImage: "terminal")
          .tag(SidebarItem.logs)
      }

      Section("Pipeline") {
        ResamplerSidebarRow()
          .tag(SidebarItem.resampler)

        ForEach(pipeline.stages.indices, id: \.self) { index in
          PipelineSidebarRow(stage: pipeline.stages[index])
            .tag(SidebarItem.stage(index))
        }
      }

      Section("EQ Presets") {
        ForEach(pipeline.eqPresets) { preset in
          EQPresetSidebarRow(preset: preset)
            .tag(SidebarItem.eqPreset(preset.id))
            .contextMenu {
              Button(role: .destructive) {
                if let idx = pipeline.eqPresets.firstIndex(where: { $0.id == preset.id }) {
                  pipeline.deleteEQPreset(at: idx)
                }
              } label: {
                Label("Delete", systemImage: "trash")
              }
            }
        }

        HStack(spacing: 12) {
          Button {
            pipeline.addEQPreset()
          } label: {
            Label("Add", systemImage: "plus")
          }

          Button {
            showOratorySearch = true
          } label: {
            Label("Oratory", systemImage: "headphones")
          }

          Button {
            showAutoEqSearch = true
          } label: {
            Label("AutoEQ", systemImage: "magnifyingglass")
          }
        }
        .foregroundStyle(.secondary)
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
  @Environment(AppState.self) var appState
  @Environment(PipelineStore.self) var pipeline

  var body: some View {
    VStack(spacing: 0) {
      switch selection {
      case .devices:
        DevicePickerView()
      case .dashboard, .none:
        DashboardView()
      case .logs:
        ConsoleLogsView()
          .environment(appState.logManager)
      case .resampler:
        ResamplerDetailView()
          .padding()
          .frame(maxHeight: .infinity, alignment: .top)
          .background(Color(nsColor: .controlBackgroundColor))

      case .eqPreset(let id):
        if let preset = pipeline.eqPresets.first(where: { $0.id == id }) {
          EQPresetDetailView(preset: preset)
        } else {
          ContentUnavailableView(
            "EQ Preset Deleted", systemImage: "slider.horizontal.3",
            description: Text("Select another preset or create a new one.")
          )
          .frame(maxHeight: .infinity)
        }

      case .stage(let index):
        StageDetailView(stageIndex: index)
      }
    }
  }
}

// MARK: - Pipeline Sidebar Rows

struct ResamplerSidebarRow: View {
  @Environment(AudioSettings.self) var settings

  var body: some View {
    @Bindable var settings = settings
    HStack {
      Image(systemName: "arrow.triangle.2.circlepath")
        .frame(width: 20)
        .foregroundStyle(settings.resamplerEnabled ? Color.accentColor : Color.secondary)
      Text("Resampler")
        .foregroundStyle(settings.resamplerEnabled ? .primary : .secondary)
      Spacer()
      Toggle("", isOn: $settings.resamplerEnabled)
        .labelsHidden()
        .toggleStyle(.switch)
        .controlSize(.mini)
    }
  }
}

struct EQPresetSidebarRow: View {
  let preset: EQPreset

  var body: some View {
    Label(preset.name, systemImage: "slider.horizontal.3")
  }
}

struct PipelineSidebarRow: View {
  @Bindable var stage: PipelineStage
  @Environment(DSPEngineController.self) var dsp

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
          dsp.applyConfig()
        }
    }
  }
}
