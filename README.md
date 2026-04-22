# CamillaDSP Monitor

A native macOS SwiftUI app for controlling and monitoring [CamillaDSP](https://github.com/HEnquist/camilladsp) — a flexible, real-time audio DSP engine for crossovers, room correction, and general audio filtering.

The app connects to a CamillaDSP process via WebSocket, providing real-time level meters, spectrum analysis, device selection, and a full pipeline configuration UI.

## Screenshots

![Dashboard](Screenshot-Dashboard.png)
![Devices](Screenshot-Device.png)
![EQ](Screenshot-EQ.png)

## Requirements

- macOS 14+ (Sonoma)
- Swift 6.0+
- A [CamillaDSP](https://github.com/HEnquist/camilladsp) binary (the app launches and manages the process automatically)

## Building

```bash
swift build
swift run CamillaDSPMonitor
```

The app automatically looks for the CamillaDSP binary in common locations (like `~/camilladsp/target/release/camilladsp`). You can also manually select a custom path in the **Device Settings** screen, which will be saved for future launches.

## Features

### Audio Device Management
- Capture and playback device selection with system default option
- Per-device sample rate picker with hardware rate change detection
- Configurable channel count and chunk size
- Exclusive (hog) mode for output devices
- Auto-refresh on device connect/disconnect

### Monitoring
- **Level meters** — Real-time RMS/Peak bars for capture and playback (L/R) via persistent WebSocket subscriptions
- **Spectrum analyzer** — 30-band 1/3-octave FFT display via independent CoreAudio tap
- **Compact level bar** — Always-visible status strip across all detail views

### Pipeline Configuration

Drag-to-reorder processing stages, each with a dedicated settings panel:

| Stage | Description |
|-------|-------------|
| Balance | L/R pan with linear pan law |
| Width | Stereo width (-100% swapped to 200% extra-wide) via Mid/Side matrix |
| M/S Proc | Mid-Side encoding at -6.02 dB |
| Phase Invert | Left / Right / Both channel polarity flip |
| Crossfeed | 5 presets (L1-L5) or custom Fc/dB with computed filter parameters |
| EQ | Same L/R or Separate L/R mode with preset selection |
| Loudness | Fletcher-Munson compensation with adjustable reference and boost |
| Emphasis | De-emphasis / Pre-emphasis highshelf filter |
| DC Protection | First-order highpass at 7 Hz |

### EQ Preset Editor

Three editing modes for parametric EQ presets:

- **Diagram** — Interactive frequency response graph with draggable color-coded band handles
- **Form** — Table-based editor with type picker, frequency, gain, and Q fields
- **CSV** — AutoEq / EqualizerAPO compatible text format with import/export

Supports 13 biquad filter types: Peaking, Lowshelf, Highshelf, Lowpass, Highpass, Notch, Bandpass, Allpass, and first-order variants.

### Resampler

Dedicated configuration panel for sample rate conversion between capture and playback devices:

- AsyncSinc with quality profiles (Very Fast / Fast / Balanced / Accurate)
- AsyncPoly with cubic interpolation
- Synchronous (fixed ratio)

### Mini Player (PIP Mode)

A floating translucent overlay visible above all windows, including full-screen video (e.g., YouTube). It is implemented as a macOS Agent app (`LSUIElement`), meaning it stays out of the Dock and functions as a persistent system utility. Three display modes are available: spectrum, pipeline signal chain, and level meters.

### Engine Control

- Start/Stop toggle in the toolbar
- Auto-recovery when CamillaDSP stalls (e.g., capture format change)
- Connection retry loop on startup (waits for CamillaDSP to be ready)
- Graceful shutdown on app termination

### Persistence

All settings saved to UserDefaults across launches: device selection, sample rates, channel counts, pipeline stage state, EQ presets, volume, and mute state.

## Architecture

```
CamillaDSPMonitor (SwiftUI)
    |
    |-- AppState (@MainActor, ObservableObject)
    |       |-- DSPEngine (actor) ---- WebSocket (Subscriptions) ----> camilladsp
    |       |-- MonitoringController (Manages VU and State subscriptions)
    |       |-- SpectrumEngine (CoreAudio tap + FFTSpectrumAnalyzer)
    |       |-- MeterState (LevelState ObservableObject, drives UI)
    |       |-- PipelineStore (ObservableObject, manages stages and presets)
    |       `-- DSPEngineController (Engine lifecycle, config building)
    |
    `-- Views (NavigationSplitView)
            |-- Dashboard (signal chain + meters + spectrum)
            |-- DevicePicker (capture/playback selection)
            |-- StageDetail (per-stage config UI)
            |-- EQPresetDetail (diagram/form/CSV modes)
            `-- MiniPlayer (NSPanel floating overlay)
```

The `DSPEngine` is a Swift actor that serializes all WebSocket communication. It manages the CamillaDSP process lifecycle and uses `AsyncStream` to provide real-time updates for engine state and VU levels.

The spectrum analyzer runs independently from CamillaDSP's signal path — it taps the capture device directly via `CoreAudioTap` and performs FFT using `vDSP`.

## Project Structure

```
Sources/
  CamillaDSPLib/
    CamillaDSP.swift            # DSPEngine actor, WebSocket protocol, data types
  CamillaDSPMonitor/
    CamillaDSPMonitorApp.swift  # @main app entry, AppDelegate
    Models/
      AppState.swift            # Central state coordinator
      MonitoringController.swift # WebSocket state/VU subscription management
      DSPEngineController.swift # Engine lifecycle and config generation
      SpectrumEngine.swift      # FFT lifecycle and CoreAudio tap driving
      MeterState.swift          # LevelState (RMS/Peak)
      PipelineStore.swift       # Stage and Preset persistence/management
      PipelineStage.swift       # Stage models and filter builders
      EQPreset.swift            # EQ band/preset models and response calculation
      AudioDeviceManager.swift  # Device enumeration and config management
      AudioSettings.swift       # Processing parameters and preferences
      FFTSpectrumAnalyzer.swift # Accelerate vDSP FFT implementation
      CoreAudioTap.swift        # CoreAudio input tap
      DeviceConfig.swift        # Device/SampleRate/Format models
      AutoEqService.swift       # AutoEq preset fetching
      LogManager.swift          # Console log collection
      DSPUtils.swift            # Math and DSP helpers
    Views/
      ContentView.swift         # NavigationSplitView and Sidebar
      DashboardView.swift       # Signal chain overview + monitoring cards
      DevicePickerView.swift    # Device and sample rate selection
      StageDetailView.swift     # Per-stage configuration panels
      EQPresetDetailView.swift  # Parametric EQ editor
      LevelMeterView.swift      # Dual RMS/Peak meter components
      SpectrumView.swift        # FFT visualization
      VolumeControlView.swift   # Toolbar volume and mute
      MiniPlayerView.swift      # Floating overlay UI
      MiniPlayerWindowController.swift # NSPanel management
      AutoEqPickerView.swift    # AutoEq search interface
      ConsoleLogsView.swift     # Real-time log viewer
```

## Dependencies

No external dependencies. Uses only Apple system frameworks:

- **SwiftUI** — UI
- **CoreAudio** — Device enumeration and hardware listeners
- **AVFoundation** — Audio engine tap for spectrum analysis
- **Accelerate** — vDSP FFT for spectrum analyzer
- **Foundation** — WebSocket (URLSessionWebSocketTask), JSON, process management

## Acknowledgments

- [CamillaDSP](https://github.com/HEnquist/camilladsp) by Henrik Enquist
- [CamillaDSP-Monitor](https://github.com/Wang-Yue/CamillaDSP-Monitor) by Wang Yue — inspiration for the monitor UI
- [camilladsp-crossfeed](https://github.com/Wang-Yue/camilladsp-crossfeed/) — crossfeed parameter computation
- Audio EQ Cookbook by Robert Bristow-Johnson — biquad coefficient formulas

## License

See [LICENSE](LICENSE).
