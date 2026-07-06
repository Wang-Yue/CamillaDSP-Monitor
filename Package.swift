// swift-tools-version:6.0
import Foundation
import PackageDescription

let engine = ProcessInfo.processInfo.environment["ENGINE"] ?? "swift"

// MARK: - Core Products List
var products: [Product] = [
  .executable(name: "DSPMonitor", targets: ["DSPMonitor"]),
  .library(name: "DSPLib", targets: ["DSPLib"]),
]

// MARK: - Engine-Specific Target and Dependency Definitions
let dspLibTarget: Target
let dspMonitorDependencies: [Target.Dependency]
let engineTargets: [Target]

switch engine {
case "swift":
  dspLibTarget = .target(
    name: "DSPLib",
    dependencies: ["DSPConfig", "SwiftDSP"],
    path: "Sources/DSPLib",
    exclude: ["RustDSPEngine.swift", "camilladsp_ffi.swift", "CDSPEngine.swift"]
  )
  dspMonitorDependencies = ["DSPLib", "DSPConfig", "SwiftDSP"]
  engineTargets = [
    .target(
      name: "SwiftDSP",
      dependencies: ["DSPConfig"],
      path: "Sources/SwiftDSP",
      linkerSettings: [
        .linkedFramework("Accelerate"),
        .linkedFramework("AudioToolbox"),
        .linkedFramework("CoreAudio"),
      ]
    ),
    .executableTarget(
      name: "RoomCorrection",
      dependencies: ["DSPLib", "DSPConfig", "SwiftDSP"],
      path: "Sources/RoomCorrection"
    ),
    .executableTarget(
      name: "DSPCLI",
      dependencies: ["DSPLib", "DSPConfig", "SwiftDSP"],
      path: "Sources/DSPCLI"
    ),
    .testTarget(
      name: "SwiftDSPTests",
      dependencies: ["DSPLib", "SwiftDSP", "DSPConfig"],
      path: "Tests/SwiftDSPTests"
    ),
  ]
  products.append(contentsOf: [
    .executable(name: "RoomCorrection", targets: ["RoomCorrection"]),
    .executable(name: "dsp-cli", targets: ["DSPCLI"]),
  ])

case "c":
  dspLibTarget = .target(
    name: "DSPLib",
    dependencies: ["DSPConfig", "CDSP"],
    path: "Sources/DSPLib",
    exclude: ["SwiftDSPEngine.swift", "RustDSPEngine.swift", "camilladsp_ffi.swift"]
  )
  dspMonitorDependencies = ["DSPLib", "DSPConfig", "CDSP"]
  engineTargets = [
    .target(
      name: "CDSP",
      path: "Sources/CDSP",
      exclude: ["main.c"],
      publicHeadersPath: ".",
      cSettings: [.headerSearchPath(".")],
      linkerSettings: [
        .linkedFramework("Accelerate"),
        .linkedFramework("AudioToolbox"),
        .linkedFramework("CoreAudio"),
        .linkedFramework("CoreFoundation"),
      ]
    )
  ]

default: // rust
  dspLibTarget = .target(
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
  )
  dspMonitorDependencies = ["DSPLib", "DSPConfig"]
  engineTargets = [
    .target(
      name: "CamillaDSPFFI",
      path: "Sources/CamillaDSPFFI"
    )
  ]
}

// MARK: - Core & Engine Targets Assembly
var targets: [Target] = [
  .target(
    name: "DSPConfig",
    dependencies: [],
    path: "Sources/DSPConfig"
  ),
  dspLibTarget,
  .executableTarget(
    name: "DSPMonitor",
    dependencies: dspMonitorDependencies,
    path: "Sources/DSPMonitor"
  ),
] + engineTargets

// MARK: - Package Definition
let package = Package(
  name: "DSPMonitor",
  platforms: [.macOS(.v15)],
  products: products,
  dependencies: [],
  targets: targets
)
