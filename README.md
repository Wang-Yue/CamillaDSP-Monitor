# CamillaDSP Monitor

A high-performance native macOS SwiftUI app for controlling and monitoring [CamillaDSP](https://github.com/HEnquist/camilladsp). Unlike traditional controllers that use WebSockets to talk to a background process, this version **integrates CamillaDSP as a native library**, providing zero-latency monitoring and superior performance.

The app uses a custom Rust bridge to embed the CamillaDSP engine directly into the Swift process, moving heavy DSP tasks (like FFT spectrum analysis) to highly optimized native code.

## Screenshots

![Dashboard](Screenshot-Dashboard.png)
![Devices](Screenshot-Device.png)
![EQ](Screenshot-EQ.png)

## Requirements

- macOS 15+ (Sequoia)
- Swift 6.0+ (Strict Concurrency enabled)
- Rust toolchain (latest stable)

## Building

The project uses a unified `Makefile` to handle the multi-language build pipeline (Rust + UniFFI + Swift).

### Build and Package as macOS Application (.app)
This will compile the Rust bridge with native CPU optimizations (`-C target-cpu=native`), generate the Swift bindings, patch them for Swift 6 concurrency, and package the final signed application:
```bash
make app          # Builds CamillaDSPMonitor.app in the root directory
make install      # Builds and copies to /Applications/
```

### Simple Build (Command Line)
```bash
make build        # Compiles everything without packaging
```

## Features

### Native Integration
- **Zero-Process Architecture** — No external `camilladsp` binary needed. The engine lives entirely inside the app's memory space.
- **High-Performance Audio Tap** — A zero-allocation circular buffer captures waveforms directly from the engine with negligible CPU overhead.
- **Optimized Rust FFT** — Spectrum analysis is performed in Rust using `realfft`, matching Apple's `vDSP` accuracy (4.0/N scaling) for perfect visual parity.

### Monitoring
- **Analog VU Meters** — Hyper-realistic, calibrated RMS/Peak needles with warm amber illumination and customizable physics.
- **Level meters** — Real-time digital RMS/Peak bars with zero-latency updates via the native bridge.
- **Spectrum analyzer** — 30-band 1/3-octave FFT display with lazy polling (only calculates and polls when the UI is visible to save battery).
- **Compact level bar** — Always-visible status strip across all detail views.

### Audio Device Management
- Capture and playback device selection with system default option
- Per-device sample rate picker with hardware rate change detection
- Configurable channel count and chunk size
- Exclusive (hog) mode for output devices
- Auto-refresh on device connect/disconnect

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

- **Diagram** — Interactive frequency response graph with draggable color-coded band handles.
- **Form** — Table-based editor with type picker, frequency, gain, and Q fields.
- **CSV** — AutoEq / EqualizerAPO compatible text format with import/export.

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
- High-performance native library initialization
- Graceful shutdown on app termination

### Persistence

All settings saved to UserDefaults across launches: device selection, sample rates, channel counts, pipeline stage state, EQ presets, volume, and mute state.

## Architecture

```
CamillaDSPMonitor (SwiftUI)
    |
    |-- AppState (@MainActor, Observable)
    |       |-- DSPEngine (actor) ---- Rust Bridge (UniFFI) ----> camilladsp lib
    |       |-- MonitoringController (Manages VU, State, and Spectrum polling)
    |       |-- SpectrumEngine (FFT data management)
    |       |-- MeterState (LevelState Observable, drives UI)
    |       |-- PipelineStore (Observable, manages stages and presets)
    |       `-- DSPEngineController (Engine lifecycle, config building)
    |
    `-- Views (NavigationSplitView)
            |-- Dashboard (signal chain + meters + spectrum)
            |-- DevicePicker (capture/playback selection)
            |-- StageDetail (per-stage config UI)
            |-- EQPresetDetail (diagram/form/CSV modes)
            `-- MiniPlayer (NSPanel floating overlay)
```

The `DSPEngine` is a Swift actor that interfaces with the integrated Rust bridge. It manages the library lifecycle and provides real-time updates for engine state and VU levels via polling.

The spectrum analyzer is implemented in Rust for maximum performance. It taps the engine's audio stream directly and performs FFT analysis, which is then polled by the Swift UI only when visible.

## Project Structure

```
Sources/
  CamillaDSPLib/
    CamillaDSP.swift            # DSPEngine actor (Native Bridge Interface)
    camilladsp_ffi.swift        # Generated UniFFI bindings
  CamillaDSPMonitor/
    CamillaDSPMonitorApp.swift  # @main app entry, AppDelegate
    Models/
      AppState.swift            # Central state coordinator
      MonitoringController.swift # State/VU/Spectrum polling management
      DSPEngineController.swift # Engine lifecycle and config generation
      SpectrumEngine.swift      # Spectrum data management
      MeterState.swift          # LevelState (RMS/Peak)
      PipelineStore.swift       # Stage and Preset persistence/management
      PipelineStage.swift       # Stage models and filter builders
      PipelineStage+Builders.swift  # Pipeline configuration builders
      PipelineStage+Defaults.swift  # Default stage configurations
      PipelineStage+Crossfeed.swift # Crossfeed parameter math
      EQPreset.swift            # EQ band/preset models and response calculation
      AudioDeviceManager.swift  # Device enumeration and config management
      AudioSettings.swift       # Processing parameters and preferences
      DeviceConfig.swift        # Device/SampleRate/Format models
      AutoEqService.swift       # AutoEq preset fetching
      LogManager.swift          # Console log collection
      DSPUtils.swift            # Math and DSP helpers
    Views/
      ContentView.swift         # NavigationSplitView and Sidebar
      DashboardView.swift       # Signal chain overview + monitoring cards
      DevicePickerView.swift    # Device and sample rate selection
      StageDetailView.swift     # Per-stage configuration panels
      EQPresetDetailView.swift  # Parametric EQ editor entry
      EQDiagramMode.swift       # Interactive EQ response graph
      EQFormMode.swift          # Table-based EQ band editor
      EQCSVMode.swift           # Text-based AutoEq/CSV editor
      AnalogVUMeterView.swift   # Hyper-realistic analog VU meter component
      LevelMeterView.swift      # Dual RMS/Peak meter components
      SpectrumView.swift        # FFT visualization
      VolumeControlView.swift   # Toolbar volume and mute
      MiniPlayerView.swift      # Floating overlay UI
      MiniPlayerContent.swift   # View content for mini player
      MiniPlayerWindowController.swift # NSPanel management
      AutoEqPickerView.swift    # AutoEq search interface
      ConsoleLogsView.swift     # Real-time log viewer
RustBridge/
  src/
    lib.rs                      # Main bridge entry
    engine.rs                   # Engine orchestration & Audio Tap
    spectrum.rs                 # FFT logic & Windowing
    types.rs                    # FFI types & Enums
  api.udl                       # UniFFI interface definition
Makefile                        # Unified build pipeline
```

## Dependencies

- **SwiftUI** — UI
- **CoreAudio** — Device enumeration and hardware listeners
- **UniFFI** — Rust/Swift bridge generation
- **CamillaDSP** — Integrated as a native library dependency
- **realfft** (Rust) — High-performance real-to-complex FFT

## Acknowledgments

- [CamillaDSP](https://github.com/HEnquist/camilladsp) by Henrik Enquist
- Audio EQ Cookbook by Robert Bristow-Johnson — biquad coefficient formulas

## License

See [LICENSE](LICENSE).
