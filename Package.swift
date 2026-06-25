// swift-tools-version:6.0
import PackageDescription

let package = Package(
  name: "DSPMonitor",
  platforms: [.macOS(.v15)],
  products: [
    .executable(name: "DSPMonitor", targets: ["DSPMonitor"]),
    .library(name: "DSPLib", targets: ["DSPLib"]),
  ],
  dependencies: [],
  targets: [
    // Core DSP Targets
    .target(
      name: "DSPConfig",
      path: "Sources/Lib/Config"
    ),
    .target(
      name: "DSPAudio",
      dependencies: [],
      path: "Sources/Lib/Audio"
    ),
    .target(
      name: "DSPLogging",
      dependencies: ["DSPConfig", "DSPAudio"],
      path: "Sources/Lib/Logging"
    ),
    .target(
      name: "DSPFFT",
      dependencies: [],
      path: "Sources/Lib/FFT",
      linkerSettings: [.linkedFramework("Accelerate")]
    ),
    .target(
      name: "DSPMixer",
      dependencies: ["DSPConfig", "DSPAudio"],
      path: "Sources/Lib/Mixer"
    ),
    .target(
      name: "DSPFilters",
      dependencies: ["DSPConfig", "DSPAudio", "DSPFFT"],
      path: "Sources/Lib/Filters",
      linkerSettings: [.linkedFramework("Accelerate")]
    ),
    .target(
      name: "DSPResampler",
      dependencies: ["DSPConfig", "DSPAudio", "DSPFFT", "DSPLogging"],
      path: "Sources/Lib/Resampler"
    ),
    .target(
      name: "DSPPipeline",
      dependencies: ["DSPConfig", "DSPAudio", "DSPFilters", "DSPMixer", "DSPLogging"],
      path: "Sources/Lib/Pipeline"
    ),
    .target(
      name: "DSPBackend",
      dependencies: ["DSPConfig", "DSPAudio", "DSPLogging"],
      path: "Sources/Lib/Backend",
      linkerSettings: [
        .linkedFramework("AudioToolbox"),
        .linkedFramework("CoreAudio"),
      ]
    ),
    .target(
      name: "DSPEngine",
      dependencies: [
        "DSPConfig", "DSPAudio", "DSPResampler", "DSPPipeline",
        "DSPBackend", "DSPLogging",
      ],
      path: "Sources/Lib/Engine"
    ),

    // App Library & Executable Targets
    .target(
      name: "DSPLib",
      dependencies: [
        "DSPConfig", "DSPAudio", "DSPBackend", "DSPEngine", "DSPFFT", "DSPFilters",
        "DSPLogging", "DSPMixer", "DSPPipeline", "DSPResampler",
      ],
      path: "Sources/DSPLib"
    ),
    .executableTarget(
      name: "DSPMonitor",
      dependencies: [
        "DSPLib",
        "DSPConfig", "DSPAudio", "DSPBackend", "DSPEngine", "DSPFFT", "DSPFilters",
        "DSPLogging", "DSPMixer", "DSPPipeline", "DSPResampler",
      ],
      path: "Sources/DSPMonitor"
    ),
    .testTarget(
      name: "DSPMonitorTests",
      dependencies: ["DSPLib"],
      path: "Tests/DSPMonitorTests"
    ),
  ]
)
