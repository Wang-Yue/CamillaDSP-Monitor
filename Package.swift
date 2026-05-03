import Foundation
// swift-tools-version:6.0
import PackageDescription

let usePureSwift = ProcessInfo.processInfo.environment["USE_PURE_SWIFT"] != "0"

var dependencies: [Package.Dependency] = []
var targets: [Target] = []

if usePureSwift {
  dependencies = [
    .package(url: "https://github.com/apple/swift-log.git", from: "1.5.0")
  ]

  var pureSwiftExcludes = ["CamillaDSP.swift"]
  if FileManager.default.fileExists(atPath: "Sources/CamillaDSPLib/camilladsp_ffi.swift") {
    pureSwiftExcludes.append("camilladsp_ffi.swift")
  }

  targets = [
    .target(
      name: "CamillaDSPLib",
      dependencies: [
        .product(name: "Logging", package: "swift-log")
      ],
      path: "Sources/CamillaDSPLib",
      exclude: pureSwiftExcludes,
      linkerSettings: [
        .linkedFramework("AudioToolbox"),
        .linkedFramework("CoreAudio"),
        .linkedFramework("Accelerate"),
      ]
    ),
    .executableTarget(
      name: "CamillaDSPMonitor",
      dependencies: [
        "CamillaDSPLib",
        .product(name: "Logging", package: "swift-log"),
      ],
      path: "Sources/CamillaDSPMonitor"
    ),
    .testTarget(
      name: "CamillaDSPLibTests",
      dependencies: ["CamillaDSPLib"],
      path: "Tests/CamillaDSPLibTests"
    ),
  ]
} else {
  targets = [
    .target(
      name: "CamillaDSPFFI",
      path: "Sources/CamillaDSPFFI"
    ),
    .target(
      name: "CamillaDSPLib",
      dependencies: ["CamillaDSPFFI"],
      path: "Sources/CamillaDSPLib",
      exclude: [
        "Audio", "Backend", "Config", "Engine",
        "Filters", "Mixer", "Pipeline", "Resampler",
      ],
      linkerSettings: [
        .linkedFramework("AudioToolbox"),
        .linkedFramework("CoreAudio"),
        .linkedFramework("Accelerate"),
        .linkedLibrary("camilladsp_ffi"),
        .unsafeFlags(["-L", "lib"]),
      ]
    ),
    .executableTarget(
      name: "CamillaDSPMonitor",
      dependencies: ["CamillaDSPLib"],
      path: "Sources/CamillaDSPMonitor"
    ),
  ]
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
