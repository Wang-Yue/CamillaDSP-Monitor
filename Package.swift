// swift-tools-version:6.0
import PackageDescription

let package = Package(
  name: "CamillaDSP",
  platforms: [.macOS(.v15)],
  products: [
    .executable(name: "CamillaDSPMonitor", targets: ["CamillaDSPMonitor"]),
    .library(name: "CamillaDSPLib", targets: ["CamillaDSPLib"]),
  ],
  dependencies: [],
  targets: [
    .target(
      name: "CamillaDSPLib",
      dependencies: [],
      path: "Sources/CamillaDSPLib",
      linkerSettings: [
        .linkedFramework("AudioToolbox"),
        .linkedFramework("CoreAudio"),
        .linkedFramework("Accelerate"),
        .linkedFramework("Security"),
      ]
    ),
    .executableTarget(
      name: "CamillaDSPMonitor",
      dependencies: ["CamillaDSPLib"],
      path: "Sources/CamillaDSPMonitor"
    ),
  ]
)
