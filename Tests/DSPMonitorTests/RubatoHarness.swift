// Lookup + invocation helpers for the rubato Rust comparison harness.
// The harness lives at `Tests/RustHarnesses/target/release/<name>` and
// is built with `make -C Tests/RustHarnesses`. Tests that depend on
// it should treat a missing binary as a soft skip — most CI runs
// don't have Cargo and shouldn't fail on that account.

import Foundation
import Testing

enum RubatoHarness {

  /// Path to the named harness binary, derived from the project root
  /// found by walking up two levels from this source file. Independent
  /// of `swift test`'s working directory.
  static func binaryPath(named name: String, file: String = #filePath) -> String {
    if let env = ProcessInfo.processInfo.environment["RUBATO_BIN"] { return env }
    let url = URL(fileURLWithPath: file)
      .deletingLastPathComponent()  // DSPMonitorTests
      .deletingLastPathComponent()  // Tests
      .appendingPathComponent("RustHarnesses/target/release/\(name)")
    return url.path
  }

  /// `true` when the resampler-comparison harness is built and
  /// executable on this machine.
  static var resamplerCompareAvailable: Bool {
    FileManager.default.isExecutableFile(
      atPath: binaryPath(named: "cdsp_resampler_compare"))
  }

  /// Run the resampler harness in the given mode. Returns `false`
  /// when the binary is missing (soft skip). Throws when invocation
  /// fails for any other reason.
  static func runResamplerCompare(
    mode: String, inputPath: String, outputPath: String,
    inRate: Int, outRate: Int, chunkSize: Int,
    noPartial: Bool = false
  ) throws -> Bool {
    let bin = binaryPath(named: "cdsp_resampler_compare")
    guard FileManager.default.isExecutableFile(atPath: bin) else {
      print(
        "⚠️ skipping: rubato harness not found at \(bin) — build with `make -C Tests/RustHarnesses`"
      )
      return false
    }
    let proc = Process()
    proc.executableURL = URL(fileURLWithPath: bin)
    var args: [String] = [
      mode, inputPath, outputPath,
      String(inRate), String(outRate), String(chunkSize),
    ]
    if noPartial { args.append("--no-partial") }
    proc.arguments = args
    let stderr = Pipe()
    proc.standardError = stderr
    try proc.run()
    proc.waitUntilExit()
    if proc.terminationStatus != 0 {
      let err =
        String(data: stderr.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
      Issue.record("rubato binary exited with \(proc.terminationStatus): \(err)")
      return false
    }
    return true
  }

  // MARK: - Raw f64 file I/O

  /// Write an array of `Double` to disk as packed little-endian f64.
  static func writeRaw(_ data: [Double], to path: String) throws {
    let buffer = data.withUnsafeBufferPointer { Data(buffer: $0) }
    try buffer.write(to: URL(fileURLWithPath: path))
  }

  /// Read a packed f64 file back into `[Double]`.
  static func readRaw(from path: String) throws -> [Double] {
    let data = try Data(contentsOf: URL(fileURLWithPath: path))
    let count = data.count / MemoryLayout<Double>.stride
    return data.withUnsafeBytes { raw -> [Double] in
      let p = raw.bindMemory(to: Double.self)
      return Array(UnsafeBufferPointer(start: p.baseAddress, count: count))
    }
  }
}
