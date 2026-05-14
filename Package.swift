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
  .target(name: "DSPDoP", dependencies: ["DSPAudio", "DSPLogging"], path: "Sources/Lib/DoP"),
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
]

let commonLibDeps: [Target.Dependency] = [
  "DSPConfig", "DSPAudio", "DSPBackend", "DSPDoP", "DSPEngine", "DSPFFT", "DSPFilters",
  "DSPLogging", "DSPMeasurement", "DSPMixer", "DSPPipeline", "DSPResampler",
]

var targets: [Target] = libTargets

if usePureSwift {
  targets.append(contentsOf: [
    .target(
      name: "CamillaDSPLib",
      dependencies: commonLibDeps,
      path: "Sources/CamillaDSPLib",
      exclude: ["RustDSPEngine.swift", "camilladsp_ffi.swift"]
    ),
    .executableTarget(
      name: "CamillaDSPMonitor",
      dependencies: ["CamillaDSPLib"] + commonLibDeps,
      path: "Sources/CamillaDSPMonitor"
    ),
    .testTarget(
      name: "CamillaDSPLibTests",
      dependencies: ["CamillaDSPLib"],
      path: "Tests/CamillaDSPLibTests"
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
      name: "CamillaDSPLib",
      dependencies: rustLibDeps,
      path: "Sources/CamillaDSPLib",
      exclude: ["SwiftDSPEngine.swift"],
      linkerSettings: [
        .linkedLibrary("camilladsp_ffi"),
        .unsafeFlags(["-L", "lib"]),
      ]
    ),
    .executableTarget(
      name: "CamillaDSPMonitor",
      dependencies: ["CamillaDSPLib"] + commonLibDeps,
      path: "Sources/CamillaDSPMonitor"
    ),
  ])
}

let package = Package(
  name: "CamillaDSP",
  platforms: [.macOS(.v15)],
  products: [
    .executable(name: "CamillaDSPMonitor", targets: ["CamillaDSPMonitor"]),
    .library(name: "CamillaDSPLib", targets: ["CamillaDSPLib"]),
  ],
  dependencies: dependencies,
  targets: targets
)
