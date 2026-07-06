// swift-tools-version:6.0
import Foundation
import PackageDescription

let engine = ProcessInfo.processInfo.environment["ENGINE"] ?? "swift"

var dependencies: [Package.Dependency] = []

let swiftLibTargets: [Target] = [
  .target(
    name: "DSPConfig",
    dependencies: [],
    path: "Sources/Lib/Config"
  ),
  .target(
    name: "DSPAudio",
    dependencies: ["DSPConfig"],
    path: "Sources/Lib/Audio"
  ),
  .target(
    name: "DSPLogging", dependencies: ["DSPConfig", "DSPAudio"], path: "Sources/Lib/Logging"),
  .target(
    name: "DSPFFT",
    dependencies: ["DSPAudio"],
    path: "Sources/Lib/FFT",
    linkerSettings: [.linkedFramework("Accelerate")]
  ),
  .target(name: "DSPMixer", dependencies: ["DSPConfig", "DSPAudio"], path: "Sources/Lib/Mixer"),
  .target(
    name: "DSPFilters",
    dependencies: ["DSPConfig", "DSPAudio", "DSPFFT"],
    path: "Sources/Lib/Filters",
    linkerSettings: [.linkedFramework("Accelerate")]
  ),
  .target(
    name: "DSPProcessors",
    dependencies: ["DSPConfig", "DSPAudio", "DSPFilters", "DSPLogging"],
    path: "Sources/Lib/Processors"
  ),
  .target(
    name: "DSPResampler",
    dependencies: ["DSPConfig", "DSPAudio", "DSPFFT", "DSPLogging"],
    path: "Sources/Lib/Resampler"
  ),
  .target(
    name: "DSPPipeline",
    dependencies: [
      "DSPConfig", "DSPAudio", "DSPFilters", "DSPMixer", "DSPLogging", "DSPProcessors",
    ],
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
    name: "DSPDoP", dependencies: ["DSPConfig", "DSPAudio", "DSPLogging"], path: "Sources/Lib/DoP"),
  .target(
    name: "DSPMeasurement",
    dependencies: ["DSPConfig", "DSPAudio", "DSPFFT", "DSPFilters", "DSPBackend"],
    path: "Sources/Lib/Measurement"
  ),
  .target(
    name: "DSPEngine",
    dependencies: [
      "DSPConfig", "DSPAudio", "DSPResampler", "DSPPipeline",
      "DSPBackend", "DSPLogging", "DSPDoP",
    ],
    path: "Sources/Lib/Engine"
  ),
  .target(
    name: "DSPServer",
    dependencies: [
      "DSPConfig", "DSPAudio", "DSPEngine", "DSPLogging",
    ],
    path: "Sources/Lib/Server"
  ),
]

let swiftCommonLibDeps: [Target.Dependency] = [
  "DSPConfig", "DSPAudio", "DSPBackend", "DSPDoP", "DSPEngine", "DSPFFT", "DSPFilters",
  "DSPLogging", "DSPMixer", "DSPPipeline", "DSPResampler", "DSPProcessors",
]

var targets: [Target] = []
var products: [Product] = [
  .executable(name: "DSPMonitor", targets: ["DSPMonitor"]),
  .library(name: "DSPLib", targets: ["DSPLib"]),
]

if engine == "swift" {
  targets.append(contentsOf: swiftLibTargets)
  targets.append(contentsOf: [
    .target(
      name: "DSPLib",
      dependencies: swiftCommonLibDeps,
      path: "Sources/DSPLib",
      exclude: ["RustDSPEngine.swift", "camilladsp_ffi.swift", "CDSPEngine.swift"]
    ),
    .executableTarget(
      name: "DSPMonitor",
      dependencies: ["DSPLib"] + swiftCommonLibDeps,
      path: "Sources/DSPMonitor"
    ),
    .executableTarget(
      name: "RoomCorrection",
      dependencies: ["DSPLib", "DSPMeasurement"] + swiftCommonLibDeps,
      path: "Sources/RoomCorrection"
    ),
    .executableTarget(
      name: "DSPCLI",
      dependencies: ["DSPLib", "DSPServer"] + swiftCommonLibDeps,
      path: "Sources/DSPCLI"
    ),
    .testTarget(
      name: "DSPMonitorTests",
      dependencies: ["DSPLib", "DSPServer", "DSPMeasurement"],
      path: "Tests/DSPMonitorTests"
    ),
  ])
  products.append(contentsOf: [
    .executable(name: "RoomCorrection", targets: ["RoomCorrection"]),
    .executable(name: "dsp-cli", targets: ["DSPCLI"]),
  ])
} else if engine == "c" {
  targets.append(contentsOf: [
    .target(
      name: "CamillaDSPCLib",
      path: "CLib",
      exclude: ["main.c"],
      publicHeadersPath: ".",
      cSettings: [.headerSearchPath(".")],
      linkerSettings: [
        .linkedFramework("Accelerate"),
        .linkedFramework("AudioToolbox"),
        .linkedFramework("CoreAudio"),
        .linkedFramework("CoreFoundation"),
      ]
    ),
    .target(
      name: "DSPConfig",
      dependencies: [],
      path: "Sources/Lib/Config"
    ),
    .target(
      name: "DSPLib",
      dependencies: ["DSPConfig", "CamillaDSPCLib"],
      path: "Sources/DSPLib",
      exclude: ["SwiftDSPEngine.swift", "RustDSPEngine.swift", "camilladsp_ffi.swift"]
    ),
    .executableTarget(
      name: "DSPMonitor",
      dependencies: ["DSPLib", "DSPConfig", "CamillaDSPCLib"],
      path: "Sources/DSPMonitor"
    ),
  ])
} else {
  targets.append(contentsOf: [
    .target(
      name: "CamillaDSPFFI",
      path: "Sources/CamillaDSPFFI"
    ),
    .target(
      name: "DSPConfig",
      dependencies: [],
      path: "Sources/Lib/Config"
    ),
    .target(
      name: "DSPLib",
      dependencies: ["CamillaDSPFFI", "DSPConfig"],
      path: "Sources/DSPLib",
      exclude: ["SwiftDSPEngine.swift", "CDSPEngine.swift"],
      linkerSettings: [
        .linkedLibrary("camilladsp_ffi"),
        .linkedFramework("AudioToolbox"),
        .linkedFramework("CoreAudio"),
        .unsafeFlags(["-L", "lib"]),
      ]
    ),
    .executableTarget(
      name: "DSPMonitor",
      dependencies: ["DSPLib", "DSPConfig"],
      path: "Sources/DSPMonitor"
    ),
  ])
}

let package = Package(
  name: "DSPMonitor",
  platforms: [.macOS(.v15)],
  products: products,
  dependencies: dependencies,
  targets: targets
)
