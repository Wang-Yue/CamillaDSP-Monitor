// swift-tools-version:6.0
import Foundation
import PackageDescription

let usePureSwift = ProcessInfo.processInfo.environment["USE_PURE_SWIFT"] != "0"

var dependencies: [Package.Dependency] = []

let libTargets: [Target] = [
  .target(name: "DSPConfig", path: "Sources/Lib/Config"),
  .target(name: "DSPAudio", dependencies: [], path: "Sources/Lib/Audio"),
  .target(
    name: "DSPLogging", dependencies: ["DSPConfig", "DSPAudio"], path: "Sources/Lib/Logging"),
  .target(
    name: "DSPFFT",
    dependencies: [],
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
    name: "DSPResampler",
    dependencies: ["DSPConfig", "DSPAudio", "DSPFFT", "DSPLogging"],
    path: "Sources/Lib/Resampler"
  ),
  .target(
    name: "DSPPipeline",
    dependencies: [
      "DSPConfig", "DSPAudio", "DSPFilters", "DSPMixer", "DSPLogging",
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
    name: "DSPEngine",
    dependencies: [
      "DSPConfig", "DSPAudio", "DSPResampler", "DSPPipeline",
      "DSPBackend", "DSPLogging", "DSPDoP",
    ],
    path: "Sources/Lib/Engine"
  ),
]

let commonLibDeps: [Target.Dependency] = [
  "DSPConfig", "DSPAudio", "DSPBackend", "DSPDoP", "DSPEngine", "DSPFFT", "DSPFilters",
  "DSPLogging", "DSPMixer", "DSPPipeline", "DSPResampler",
]

var targets: [Target] = libTargets

if usePureSwift {
  targets.append(contentsOf: [
    .target(
      name: "DSPLib",
      dependencies: commonLibDeps,
      path: "Sources/DSPLib",
      exclude: ["RustDSPEngine.swift", "camilladsp_ffi.swift"]
    ),
    .executableTarget(
      name: "DSPMonitor",
      dependencies: ["DSPLib"] + commonLibDeps,
      path: "Sources/DSPMonitor"
    ),
    .testTarget(
      name: "DSPMonitorTests",
      dependencies: ["DSPLib"],
      path: "Tests/DSPMonitorTests"
    ),
  ])
} else {
  var rustLibDeps = commonLibDeps
  rustLibDeps.append("CamillaDSPFFI")

  targets.append(contentsOf: [
    .target(
      name: "CamillaDSPFFI",
      path: "Sources/CamillaDSPFFI"
    ),
    .target(
      name: "DSPLib",
      dependencies: rustLibDeps,
      path: "Sources/DSPLib",
      exclude: ["SwiftDSPEngine.swift"],
      linkerSettings: [
        .linkedLibrary("camilladsp_ffi"),
        .unsafeFlags(["-L", "lib"]),
      ]
    ),
    .executableTarget(
      name: "DSPMonitor",
      dependencies: ["DSPLib"] + commonLibDeps,
      path: "Sources/DSPMonitor"
    ),
  ])
}

let package = Package(
  name: "DSPMonitor",
  platforms: [.macOS(.v15)],
  products: [
    .executable(name: "DSPMonitor", targets: ["DSPMonitor"]),
    .library(name: "DSPLib", targets: ["DSPLib"]),
  ],
  dependencies: dependencies,
  targets: targets
)
