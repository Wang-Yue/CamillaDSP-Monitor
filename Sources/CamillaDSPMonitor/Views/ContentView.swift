// ContentView - Main app layout with sidebar navigation and detail panel

import Observation
import SwiftUI

struct ContentView: View {
  @Environment(AppState.self) var appState
  @State private var selection: SidebarItem? = .devices
  @State private var showAutoEqSearch = false

  var body: some View {
    if appState.isMiniPlayerActive {
      Color.clear
    } else {
      NavigationSplitView {
        SidebarView(selection: $selection, showAutoEqSearch: $showAutoEqSearch)
      } detail: {
        DetailPanel(selection: selection)
      }
      .toolbar {
        ToolbarView()
      }
      .navigationTitle("CamillaDSP Monitor")
      .sheet(isPresented: $showAutoEqSearch) {
        AutoEqPickerView()
          .environment(appState.pipeline)
      }
    }
  }
}

// MARK: - Sidebar Item

enum SidebarItem: Hashable {
  case devices
  case levels
  case spectrum
  case spectroscope
  case vectorscope
  case analogVU
  case logs
  case dashboard
  case resampler
  case stage(Int)
  case eqPreset(Int)
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

  var body: some View {
    @Bindable var appState = appState
    List(selection: $selection) {
      Section("Audio") {
        Label("Devices", systemImage: "hifispeaker.2")
          .tag(SidebarItem.devices)
        Label("Dashboard", systemImage: "gauge.open.with.lines.needle.33percent")
          .tag(SidebarItem.dashboard)
      }

      Section("Monitoring") {
        MonitoringSidebarRow(
          icon: "chart.bar", title: "Level Meters", isEnabled: $appState.showLevelMetersInDashboard
        )
        .tag(SidebarItem.levels)
        MonitoringSidebarRow(
          icon: "waveform.path.ecg.rectangle", title: "Spectrum",
          isEnabled: $appState.showSpectrumInDashboard
        )
        .tag(SidebarItem.spectrum)
        MonitoringSidebarRow(
          icon: "circle.grid.3x3.fill", title: "Spectroscope",
          isEnabled: $appState.showSpectrogramInDashboard
        )
        .tag(SidebarItem.spectroscope)
        MonitoringSidebarRow(
          icon: "waveform.path", title: "Vector Scope",
          isEnabled: $appState.showVectorScopeInDashboard
        )
        .tag(SidebarItem.vectorscope)
        MonitoringSidebarRow(
          icon: "gauge.with.needle", title: "Analog VU",
          isEnabled: $appState.showAnalogVUInDashboard
        )
        .tag(SidebarItem.analogVU)
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
  @Environment(AppState.self) var appState
  @Environment(PipelineStore.self) var pipeline

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
        SpectrumDetailView()
      case .spectroscope:
        SpectroscopeDetailView()
      case .vectorscope:
        VectorScopeDetailView()
      case .analogVU:
        AnalogVUDetailView()  // Use a specialized detail view that includes controls
      case .logs:
        ConsoleLogsView()
          .environment(appState.logManager)
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

// MARK: - Analog VU Detail (with interactive controls)

struct AnalogVUDetailView: View {
  @Environment(VUSettings.self) var vuSettings

  var body: some View {
    @Bindable var vuSettings = vuSettings
    VStack(spacing: 0) {
      ScrollView {
        AnalogVUCard()
          .padding(32)
      }

      Divider()

      // PERSISTENT CALIBRATION CONTROLS
      VStack(alignment: .leading, spacing: 16) {
        HStack {
          Label("VU Calibration & Lighting", systemImage: "slider.horizontal.3")
            .font(.headline)
          Spacer()
          Button("Reset to Defaults") {
            vuSettings.reset()
          }
          .buttonStyle(.bordered)
          .controlSize(.small)
        }

        Grid(alignment: .leading, horizontalSpacing: 32, verticalSpacing: 16) {
          GridRow {
            VStack(alignment: .leading, spacing: 4) {
              Text("Scale Radius").font(.caption).foregroundStyle(.secondary)
              HStack {
                Slider(value: $vuSettings.radiusScale, in: 1.0...1.5)
                Text(String(format: "%.2f", vuSettings.radiusScale)).font(
                  .system(.body, design: .monospaced)
                ).frame(width: 45)
              }
            }
            VStack(alignment: .leading, spacing: 4) {
              Text("Pivot Position (Y)").font(.caption).foregroundStyle(.secondary)
              HStack {
                Slider(value: $vuSettings.pivotY, in: 1.0...2.0)
                Text(String(format: "%.2f", vuSettings.pivotY)).font(
                  .system(.body, design: .monospaced)
                ).frame(width: 45)
              }
            }
          }

          GridRow {
            VStack(alignment: .leading, spacing: 4) {
              Text("Needle Extension").font(.caption).foregroundStyle(.secondary)
              HStack {
                Slider(value: $vuSettings.needleExtension, in: 0...60)
                Text(String(format: "%.1f", vuSettings.needleExtension)).font(
                  .system(.body, design: .monospaced)
                ).frame(width: 45)
              }
            }
            VStack(alignment: .leading, spacing: 4) {
              Text("Ambient Glow").font(.caption).foregroundStyle(.secondary)
              HStack {
                Slider(value: $vuSettings.ambientGlow, in: 0.0...1.0)
                Text(String(format: "%.2f", vuSettings.ambientGlow)).font(
                  .system(.body, design: .monospaced)
                ).frame(width: 45)
              }
            }
          }

          GridRow {
            VStack(alignment: .leading, spacing: 4) {
              Text("Focused Hot Spot").font(.caption).foregroundStyle(.secondary)
              HStack {
                Slider(value: $vuSettings.hotSpotAlpha, in: 0.0...1.0)
                Text(String(format: "%.2f", vuSettings.hotSpotAlpha)).font(
                  .system(.body, design: .monospaced)
                ).frame(width: 45)
              }
            }
            VStack(alignment: .leading, spacing: 4) {
              Text("Overall Light Wash").font(.caption).foregroundStyle(.secondary)
              HStack {
                Slider(value: $vuSettings.lightWash, in: 0.0...0.4)
                Text(String(format: "%.2f", vuSettings.lightWash)).font(
                  .system(.body, design: .monospaced)
                ).frame(width: 45)
              }
            }
          }
        }
      }
      .padding(24)
      .background(.thinMaterial)
    }
    .background(Color(nsColor: .controlBackgroundColor))
  }
}

// MARK: - Spectrum Detail (with interactive controls)

struct SpectrumDetailView: View {
  @Environment(SpectrumEngine.self) var spectrum

  var body: some View {
    @Bindable var spectrum = spectrum
    VStack(spacing: 0) {
      ScrollView {
        SpectrumCard()
          .padding(32)
      }

      Divider()

      // SPECTRUM SETTINGS CONTROLS
      VStack(alignment: .leading, spacing: 16) {
        HStack {
          Label("Spectrum Settings", systemImage: "slider.horizontal.3")
            .font(.headline)
          Spacer()
          Button("Reset to Defaults") {
            spectrum.resetToDefaults()
          }
          .buttonStyle(.bordered)
          .controlSize(.small)
        }

        HStack(spacing: 20) {
          VStack(alignment: .leading, spacing: 4) {
            Text("Source").font(.caption).foregroundStyle(.secondary)
            Picker("", selection: $spectrum.isCapture) {
              Text("Capture").tag(true)
              Text("Playback").tag(false)
            }
            .pickerStyle(.segmented)
            .frame(width: 140)
          }

          VStack(alignment: .leading, spacing: 4) {
            Text("Bins").font(.caption).foregroundStyle(.secondary)
            Stepper("\(Int(spectrum.nBins))", value: $spectrum.nBins, in: 2...100)
              .frame(width: 100)
          }

          VStack(alignment: .leading, spacing: 4) {
            Text("Range: \(Int(spectrum.minFreq)) - \(Int(spectrum.maxFreq)) Hz").font(.caption)
              .foregroundStyle(.secondary)
            LogRangeSlider(
              minValue: $spectrum.minFreq, maxValue: $spectrum.maxFreq, range: 20...20000
            )
            .frame(maxWidth: .infinity)
          }

          Spacer()
        }
        .padding(.vertical, 8)
      }
      .padding(24)
      .background(.thinMaterial)
    }
    .background(Color(nsColor: .controlBackgroundColor))
  }
}

// MARK: - Spectroscope Detail (with interactive controls)

struct SpectroscopeDetailView: View {
  @Environment(SpectrogramEngine.self) var spectroscope

  var body: some View {
    @Bindable var spectroscope = spectroscope
    VStack(spacing: 0) {
      ScrollView {
        SpectrogramCard()
          .padding(32)
      }

      Divider()

      // SPECTROSCOPE SETTINGS CONTROLS
      VStack(alignment: .leading, spacing: 16) {
        HStack {
          Label("Spectroscope Settings", systemImage: "slider.horizontal.3")
            .font(.headline)
          Spacer()
          Button("Reset to Defaults") {
            spectroscope.resetToDefaults()
          }
          .buttonStyle(.bordered)
          .controlSize(.small)
        }

        HStack(spacing: 20) {
          VStack(alignment: .leading, spacing: 4) {
            Text("Source").font(.caption).foregroundStyle(.secondary)
            Picker("", selection: $spectroscope.isCapture) {
              Text("Capture").tag(true)
              Text("Playback").tag(false)
            }
            .pickerStyle(.segmented)
            .frame(width: 140)
          }

          VStack(alignment: .leading, spacing: 4) {
            Text("Bins").font(.caption).foregroundStyle(.secondary)
            Stepper(
              "\(Int(spectroscope.nBins))", value: $spectroscope.nBins, in: 20...500, step: 20
            )
            .frame(width: 120)
          }

          Spacer()
        }
        .padding(.vertical, 8)
      }
      .padding(24)
      .background(.thinMaterial)
    }
    .background(Color(nsColor: .controlBackgroundColor))
  }
}

// MARK: - Vector Scope Detail (with interactive controls)

struct VectorScopeDetailView: View {
  @Environment(VectorScopeEngine.self) var vectorscope

  var body: some View {
    @Bindable var vectorscope = vectorscope
    VStack(spacing: 0) {
      VectorScopeView()
        .padding(32)

      Divider()

      // VECTOR SCOPE SETTINGS CONTROLS
      VStack(alignment: .leading, spacing: 16) {
        HStack {
          Label("Vector Scope Settings", systemImage: "slider.horizontal.3")
            .font(.headline)
          Spacer()
          Button("Reset to Defaults") {
            vectorscope.resetToDefaults()
          }
          .buttonStyle(.bordered)
          .controlSize(.small)
        }

        HStack(spacing: 20) {
          VStack(alignment: .leading, spacing: 4) {
            Text("Source").font(.caption).foregroundStyle(.secondary)
            Picker("", selection: $vectorscope.isCapture) {
              Text("Capture").tag(true)
              Text("Playback").tag(false)
            }
            .pickerStyle(.segmented)
            .frame(width: 140)
          }

          VStack(alignment: .leading, spacing: 4) {
            Text("Frames").font(.caption).foregroundStyle(.secondary)
            Stepper(
              "\(Int(vectorscope.nFrames))", value: $vectorscope.nFrames, in: 128...4096, step: 128
            )
            .frame(width: 140)
          }

          Spacer()
        }
        .padding(.vertical, 8)
      }
      .padding(24)
      .background(.thinMaterial)
    }
    .background(Color(nsColor: .controlBackgroundColor))
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

struct MonitoringSidebarRow: View {
  let icon: String
  let title: String
  @Binding var isEnabled: Bool

  var body: some View {
    HStack {
      Image(systemName: icon)
        .frame(width: 20)
        .foregroundStyle(isEnabled ? Color.accentColor : Color.secondary)
      Text(title)
        .foregroundStyle(isEnabled ? .primary : .secondary)
      Spacer()
      Toggle("", isOn: $isEnabled)
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
