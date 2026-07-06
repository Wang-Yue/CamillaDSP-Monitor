// swift-tools-version:6.0
import Foundation
import PackageDescription

let engine = ProcessInfo.processInfo.environment["ENGINE"] ?? "swift"

var dependencies: [Package.Dependency] = []

let swiftLibTargets: [Target] = [
  .target(
    name: "DSPConfig",
    dependencies: [],
    path: "Sources/DSPConfig"
  ),
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
      dependencies: ["DSPConfig", "SwiftDSP"],
      path: "Sources/DSPLib",
      exclude: ["RustDSPEngine.swift", "camilladsp_ffi.swift", "CDSPEngine.swift"]
    ),
    .executableTarget(
      name: "DSPMonitor",
      dependencies: ["DSPLib", "DSPConfig", "SwiftDSP"],
      path: "Sources/DSPMonitor"
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
  ])
  products.append(contentsOf: [
    .executable(name: "RoomCorrection", targets: ["RoomCorrection"]),
    .executable(name: "dsp-cli", targets: ["DSPCLI"]),
  ])
} else if engine == "c" {
  targets.append(contentsOf: [
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
    ),
    .target(
      name: "DSPConfig",
      dependencies: [],
      path: "Sources/DSPConfig"
    ),
    .target(
      name: "DSPLib",
      dependencies: ["DSPConfig", "CDSP"],
      path: "Sources/DSPLib",
      exclude: ["SwiftDSPEngine.swift", "RustDSPEngine.swift", "camilladsp_ffi.swift"]
    ),
    .executableTarget(
      name: "DSPMonitor",
      dependencies: ["DSPLib", "DSPConfig", "CDSP"],
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
      path: "Sources/DSPConfig"
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
