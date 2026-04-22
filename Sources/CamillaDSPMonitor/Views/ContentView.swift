// ContentView - Main app layout with sidebar navigation and detail panel

import SwiftUI

struct ContentView: View {
  @EnvironmentObject var appState: AppState
  @State private var selection: SidebarItem? = .devices
  @State private var showAutoEqSearch = false

  var body: some View {
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
      }
      .toolbar {
        ToolbarView()
      }
      .navigationTitle("CamillaDSP Monitor")
      .sheet(isPresented: $showAutoEqSearch) {
        AutoEqPickerView()
          .environmentObject(appState.pipeline)
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
  @EnvironmentObject var dsp: DSPEngineController
  @EnvironmentObject var devices: AudioDeviceManager

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

      VolumeControlView()
    }
  }
}

// MARK: - Sidebar

struct SidebarView: View {
  @EnvironmentObject var settings: AudioSettings
  @EnvironmentObject var pipeline: PipelineStore
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

        ForEach(pipeline.stages.indices, id: \.self) { index in
          PipelineSidebarRow(stage: pipeline.stages[index])
            .tag(SidebarItem.stage(index))
        }
      }

      Section("EQ Presets") {
        ForEach(pipeline.eqPresets.indices, id: \.self) { index in
          EQPresetSidebarRow(preset: pipeline.eqPresets[index])
            .tag(SidebarItem.eqPreset(index))
            .contextMenu {
              Button(role: .destructive) {
                pipeline.deleteEQPreset(at: index)
              } label: {
                Label("Delete", systemImage: "trash")
              }
            }
        }

        HStack {
          Button {
            pipeline.addEQPreset()
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
  @EnvironmentObject var pipeline: PipelineStore

  var body: some View {
    VStack(spacing: 0) {
      CompactLevelMeterBar()
        .padding(.horizontal)
        .padding(.vertical, 8)

      Divider()

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
        if index < pipeline.eqPresets.count {
          EQPresetDetailView(preset: pipeline.eqPresets[index])
        }
      case .stage(let index):
        StageDetailView(stageIndex: index)
      }
    }
  }
}

// MARK: - Pipeline Sidebar Rows

struct ResamplerSidebarRow: View {
  @EnvironmentObject var settings: AudioSettings

  var body: some View {
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
  @ObservedObject var preset: EQPreset

  var body: some View {
    Label(preset.name, systemImage: "slider.horizontal.3")
  }
}

struct PipelineSidebarRow: View {
  @ObservedObject var stage: PipelineStage
  @EnvironmentObject var dsp: DSPEngineController

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
