import DSPAudio
import DSPBackend
import DSPConfig
import DSPFilters
import Foundation

@MainActor
final class ConvolutionImportService {
  static let shared = ConvolutionImportService()

  private init() {}

  struct ImportItem: Identifiable, Equatable {
    let id: UUID
    var fileURL: URL
    var sampleRate: Int
    var format: String  // "WAV", "FLOAT64", "FLOAT32", "S16_LE", "S32_LE", "TEXT"
    var channel: Int  // 0-based channel index, only used for WAV files

    init(fileURL: URL, sampleRate: Int, format: String, channel: Int = 0) {
      self.id = UUID()
      self.fileURL = fileURL
      self.sampleRate = sampleRate
      self.format = format
      self.channel = channel
    }
  }

  /// Supported rates for UI picker (filtered to standard rates >= 32 kHz).
  static var standardRates: [Int] {
    CoreAudioCapabilities.standardRates.filter { $0 >= 32000 }
  }

  /// Supported formats for UI picker.
  static let formats = ["WAV", "FLOAT64", "FLOAT32", "S16_LE", "S32_LE", "TEXT"]

  /// Infer sample rate and format from a file URL.
  func inferMetadata(for url: URL) -> (sampleRate: Int, format: String) {
    let filename = url.lastPathComponent.lowercased()
    var inferredRate = 48000
    var inferredFormat = "FLOAT64"

    // 1. Detect format by extension
    if filename.hasSuffix(".wav") {
      inferredFormat = "WAV"
      // Try reading WAV header to get exact sample rate
      if let data = try? Data(contentsOf: url, options: .mappedIfSafe), data.count > 28 {
        let rate = data.withUnsafeBytes { $0.load(fromByteOffset: 24, as: UInt32.self) }
        if rate >= 8000 && rate <= 384000 {
          inferredRate = Int(rate)
        }
      }
    } else if filename.hasSuffix(".txt") {
      inferredFormat = "TEXT"
    } else if filename.hasSuffix(".f32") || filename.contains("float32") || filename.contains("f32")
    {
      inferredFormat = "FLOAT32"
    } else if filename.hasSuffix(".f64") || filename.contains("float64") || filename.contains("f64")
    {
      inferredFormat = "FLOAT64"
    }

    // 2. Infer rate from filename if not already populated from WAV header
    if inferredFormat != "WAV" {
      let rates = CoreAudioCapabilities.standardRates.sorted(by: >)
      for rate in rates {
        if filename.contains("\(rate)") || filename.contains("\(rate / 1000)k")
          || filename.contains("\(Double(rate) / 1000.0)k")
        {
          inferredRate = rate
          break
        }
      }
    }

    return (inferredRate, inferredFormat)
  }

  /// Loads the specified files, standardizes them, sandboxes them under `~/Library/Application Support/CamillaDSP-Monitor/IRs/`,
  /// and registers a new `ConvolutionPreset` in the `PipelineStore`.
  func importPreset(
    name: String,
    kindLabel: String,
    items: [ImportItem],
    pipeline: PipelineStore
  ) throws -> ConvolutionPreset {
    guard !items.isEmpty else {
      throw NSError(
        domain: "ConvolutionImportService", code: 1,
        userInfo: [NSLocalizedDescriptionKey: "No files selected for import."])
    }

    let fm = FileManager.default
    let appSupport = try fm.url(
      for: .applicationSupportDirectory, in: .userDomainMask,
      appropriateFor: nil, create: true)
    let dir =
      appSupport
      .appendingPathComponent("CamillaDSP-Monitor", isDirectory: true)
      .appendingPathComponent("IRs", isDirectory: true)
    try fm.createDirectory(at: dir, withIntermediateDirectories: true)

    let presetID = UUID()
    var irPaths: [Int: String] = [:]
    var firstTapCount = 0

    for item in items {
      // 1. Load time-domain coefficients using ConvCoefficientLoader
      let path = item.fileURL.path
      let coeffs: [PrcFmt]
      if item.format == "WAV" {
        coeffs = try ConvCoefficientLoader.loadWAV(path: path, channel: item.channel)
      } else {
        coeffs = try ConvCoefficientLoader.loadRaw(path: path, format: item.format)
      }

      guard !coeffs.isEmpty else {
        throw NSError(
          domain: "ConvolutionImportService", code: 2,
          userInfo: [
            NSLocalizedDescriptionKey:
              "File '\(item.fileURL.lastPathComponent)' contains zero coefficients."
          ])
      }

      if firstTapCount == 0 {
        firstTapCount = coeffs.count
      }

      // 2. Save standard double-precision little-endian raw floats (.f64) to the persistent directory
      let destURL = dir.appendingPathComponent(
        "Imported-\(presetID.uuidString.prefix(8))-\(item.sampleRate).f64")
      let data = coeffs.withUnsafeBufferPointer { (buf: UnsafeBufferPointer<PrcFmt>) -> Data in
        Data(buffer: buf)
      }
      try data.write(to: destURL, options: [.atomic])

      irPaths[item.sampleRate] = destURL.path
    }

    // 3. Create preset and add to pipeline
    let preset = ConvolutionPreset(
      name: name.trimmingCharacters(in: .whitespacesAndNewlines),
      irPaths: irPaths,
      taps: firstTapCount,
      kindLabel: kindLabel.trimmingCharacters(in: .whitespacesAndNewlines)
    )

    pipeline.addConvolutionPreset(preset)
    return preset
  }
}
