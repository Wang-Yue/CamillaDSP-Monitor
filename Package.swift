// swift-tools-version:6.0
import Foundation
import PackageDescription

let engine = ProcessInfo.processInfo.environment["ENGINE"] ?? "c"

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
case "c":
  dspLibTarget = .target(
    name: "DSPLib",
    dependencies: [
      "DSPConfig",
      .product(name: "CDSP", package: "CDSP")
    ],
    path: "Sources/DSPLib",
    exclude: ["RustDSPEngine.swift", "camilladsp_ffi.swift"],
    swiftSettings: [
      .unsafeFlags(["-Xcc", "-DENABLE_COREAUDIO"]),
      .unsafeFlags(["-Xcc", "-DENABLE_ACCELERATE"]),
    ]
  )
  dspMonitorDependencies = [
    "DSPLib",
    "DSPConfig",
    .product(name: "CDSP", package: "CDSP")
  ]
  engineTargets = []

default:  // rust
  dspLibTarget = .target(
    name: "DSPLib",
    dependencies: ["CamillaDSPFFI", "DSPConfig"],
    path: "Sources/DSPLib",
    exclude: ["CDSPEngine.swift"],
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
var targets: [Target] =
  [
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
    .testTarget(
      name: "RoomCorrectionTests",
      dependencies: ["DSPLib", "DSPConfig", "DSPMonitor"],
      path: "Tests/RoomCorrectionTests"
    ),
  ] + engineTargets

// MARK: - Package Definition
let package = Package(
  name: "DSPMonitor",
  platforms: [.macOS(.v15)],
  products: products,
  dependencies: [
    .package(url: "https://github.com/Wang-Yue/cdsp.git", branch: "main")
  ],
  targets: targets
)
