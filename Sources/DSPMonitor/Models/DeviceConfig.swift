import DSPConfig
import Foundation

/// Combined device config: selected device name, channel/rate/format, and fetched capabilities.
/// `capabilities.name == ""` means system default (no specific device selected).
/// `enforced()` cascades any out-of-range selection down to the nearest valid value.
public struct DeviceConfig: Equatable, Sendable, Codable {
  public var backend: AudioBackendType
  public var capabilities: AudioDeviceDescriptor

  public var channels: Int
  public var deviceChannels: Int
  public var sampleRate: Int
  public var format: String
  public var bypassDoP: Bool
  /// DoP decimator passband cutoff in Hz. 20 kHz keeps SINAD highest;
  /// 30–50 kHz widens the audible passband at modest SINAD cost. Ignored
  /// when `bypassDoP` is true.
  public var dopCutoffHz: Double

  /// Whether to encode output PCM into DSD-over-PCM (DoP)
  public var outputDoP: Bool
  /// Selected sigma-delta modulator noise-shaping filter or "auto"
  public var dsdEncoderFilter: SDMFilter

  // File Backend Settings
  public var filename: String
  public var fileFormat: String
  public var isWav: Bool
  public var useRf64: Bool
  public var skipBytes: Int
  public var readBytes: Int
  public var extraSamples: Int

  // Generator Backend Settings
  public var generatorType: String
  public var generatorFreq: Double
  public var generatorLevel: Double

  /// `nil` -> system default (capabilities.name is "").
  /// Setting this replaces capabilities with a bare descriptor (capability_sets cleared),
  /// signalling that a fetch is needed.
  public var deviceName: String? {
    get {
      capabilities.name.isEmpty ? nil : capabilities.name
    }
    set {
      let newName = newValue ?? ""
      guard capabilities.name != newName else { return }
      capabilities = AudioDeviceDescriptor(
        name: newName, capability_sets: [])
    }
  }

  public init() {
    self.backend = .coreAudio
    self.capabilities = AudioDeviceDescriptor()
    self.channels = 2
    self.deviceChannels = 2
    self.sampleRate = 48000
    self.format = "F32"
    self.bypassDoP = true
    self.dopCutoffHz = 20_000
    self.outputDoP = false
    self.dsdEncoderFilter = .sdm6
    self.filename = ""
    self.fileFormat = "S16_LE"
    self.isWav = false
    self.useRf64 = false
    self.skipBytes = 0
    self.readBytes = 0
    self.extraSamples = 0
    self.generatorType = "Sine"
    self.generatorFreq = 1000.0
    self.generatorLevel = -6.0
  }

  // Custom decode tolerates configs persisted before new fields existed.
  private enum CodingKeys: String, CodingKey {
    case backend, capabilities, channels, deviceChannels, sampleRate, format, bypassDoP,
      dopCutoffHz,
      outputDoP, dsdEncoderFilter, filename, fileFormat, isWav, useRf64, skipBytes, readBytes, extraSamples,
      generatorType, generatorFreq, generatorLevel
  }

  public init(from decoder: Decoder) throws {
    let c = try decoder.container(keyedBy: CodingKeys.self)
    self.backend = try c.decodeIfPresent(AudioBackendType.self, forKey: .backend) ?? .coreAudio
    self.capabilities = try c.decode(AudioDeviceDescriptor.self, forKey: .capabilities)
    self.channels = try c.decode(Int.self, forKey: .channels)
    self.deviceChannels = try c.decodeIfPresent(Int.self, forKey: .deviceChannels) ?? self.channels
    self.sampleRate = try c.decode(Int.self, forKey: .sampleRate)
    self.format = try c.decode(String.self, forKey: .format)
    self.bypassDoP = try c.decode(Bool.self, forKey: .bypassDoP)
    self.dopCutoffHz = try c.decodeIfPresent(Double.self, forKey: .dopCutoffHz) ?? 20_000
    self.outputDoP = try c.decodeIfPresent(Bool.self, forKey: .outputDoP) ?? false
    self.dsdEncoderFilter =
      try c.decodeIfPresent(SDMFilter.self, forKey: .dsdEncoderFilter) ?? .sdm6
    self.filename = try c.decodeIfPresent(String.self, forKey: .filename) ?? ""
    self.fileFormat = try c.decodeIfPresent(String.self, forKey: .fileFormat) ?? "S16_LE"
    self.isWav = try c.decodeIfPresent(Bool.self, forKey: .isWav) ?? false
    self.useRf64 = try c.decodeIfPresent(Bool.self, forKey: .useRf64) ?? false
    self.skipBytes = try c.decodeIfPresent(Int.self, forKey: .skipBytes) ?? 0
    self.readBytes = try c.decodeIfPresent(Int.self, forKey: .readBytes) ?? 0
    self.extraSamples = try c.decodeIfPresent(Int.self, forKey: .extraSamples) ?? 0
    self.generatorType = try c.decodeIfPresent(String.self, forKey: .generatorType) ?? "Sine"
    self.generatorFreq = try c.decodeIfPresent(Double.self, forKey: .generatorFreq) ?? 1000.0
    self.generatorLevel = try c.decodeIfPresent(Double.self, forKey: .generatorLevel) ?? -6.0
  }

  public func encode(to encoder: Encoder) throws {
    var c = encoder.container(keyedBy: CodingKeys.self)
    try c.encode(backend, forKey: .backend)
    try c.encode(capabilities, forKey: .capabilities)
    try c.encode(channels, forKey: .channels)
    try c.encode(deviceChannels, forKey: .deviceChannels)
    try c.encode(sampleRate, forKey: .sampleRate)
    try c.encode(format, forKey: .format)
    try c.encode(bypassDoP, forKey: .bypassDoP)
    try c.encode(dopCutoffHz, forKey: .dopCutoffHz)
    try c.encode(outputDoP, forKey: .outputDoP)
    try c.encode(dsdEncoderFilter, forKey: .dsdEncoderFilter)
    try c.encode(filename, forKey: .filename)
    try c.encode(fileFormat, forKey: .fileFormat)
    try c.encode(isWav, forKey: .isWav)
    try c.encode(useRf64, forKey: .useRf64)
    try c.encode(skipBytes, forKey: .skipBytes)
    try c.encode(readBytes, forKey: .readBytes)
    try c.encode(extraSamples, forKey: .extraSamples)
    try c.encode(generatorType, forKey: .generatorType)
    try c.encode(generatorFreq, forKey: .generatorFreq)
    try c.encode(generatorLevel, forKey: .generatorLevel)
  }

  // MARK: - Capabilities Logic

  private static func formatPriority(for fmt: String) -> Int {
    if fmt == "S32" || fmt == "S32_LE" || fmt == "S32_BE" { return 7 }
    if fmt == "S24" || fmt.hasPrefix("S24_4") { return 6 }
    if fmt.hasPrefix("S24_3") { return 5 }
    if fmt == "S16" || fmt == "S16_LE" || fmt == "S16_BE" { return 4 }
    if fmt == "F32" || fmt == "F32_LE" || fmt == "F32_BE" { return 3 }
    if fmt == "F64" || fmt == "F64_LE" || fmt == "F64_BE" { return 2 }
    if fmt.hasPrefix("DSD") { return 1 }
    return 0
  }

  /// Channel counts this device supports, sorted ascending.
  public var supportedChannels: [Int] {
    capabilities.capability_sets.first?.capabilities.map { $0.channels }.sorted() ?? []
  }

  /// Supported sample rates for a given channel count.
  /// Falls back to the union across all channel counts if the count is not found.
  public var supportedRates: [Int] {
    guard let set = capabilities.capability_sets.first else { return [] }
    let cap =
      set.capabilities.first(where: { $0.channels == deviceChannels }) ?? set.capabilities.first
    let rates: [Int]
    if let cap = cap {
      rates = cap.samplerates.map { $0.samplerate }
    } else {
      rates = set.capabilities.flatMap { $0.samplerates.map { $0.samplerate } }
    }
    return Set(rates).sorted()
  }

  /// Available sample formats for a given channel count and sample rate, sorted best-first.
  public var supportedFormats: [String] {
    guard let set = capabilities.capability_sets.first else { return [] }
    let cap =
      set.capabilities.first(where: { $0.channels == deviceChannels }) ?? set.capabilities.first
    let formats = cap?.samplerates.first(where: { $0.samplerate == sampleRate })?.formats ?? []
    return formats.sorted { Self.formatPriority(for: $0) > Self.formatPriority(for: $1) }
  }

  /// Returns a copy with channels/rate/format snapped to supported values.
  /// Pure function - no side effects.
  public func enforced() -> DeviceConfig {
    var result = self
    if result.backend == .coreAudio {
      let ch = result.supportedChannels
      if !ch.isEmpty {
        let devChValid = ch.contains(result.deviceChannels) && result.deviceChannels >= result.channels
        if !devChValid {
          if let bestPhys = ch.first(where: { $0 >= result.channels }) {
            result.deviceChannels = bestPhys
          } else {
            let maxPhys = ch.max() ?? 2
            result.channels = maxPhys
            result.deviceChannels = maxPhys
          }
        }
      }
      result.channels = max(1, min(result.deviceChannels, result.channels))

      let rates = result.supportedRates
      if !rates.isEmpty && !rates.contains(result.sampleRate) {
        result.sampleRate = Self.bestRate(from: rates, preferring: result.sampleRate)
      }
      let fmts = result.supportedFormats
      if !fmts.isEmpty && !fmts.contains(result.format) {
        result.format = fmts.first ?? "F32"
      }
    } else if result.backend == .wavFile {
      if !result.filename.isEmpty {
        if let wavInfo = Self.parseWavHeader(atPath: result.filename) {
          result.channels = wavInfo.channels
          result.sampleRate = wavInfo.sampleRate
        }
      }
      result.channels = max(1, min(32, result.channels))
      result.deviceChannels = result.channels
    } else {
      result.channels = max(1, min(32, result.channels))
      result.deviceChannels = result.channels
    }
    return result
  }

  public static func parseWavHeader(atPath path: String) -> (channels: Int, sampleRate: Int)? {
    guard !path.isEmpty else { return nil }
    let url = URL(fileURLWithPath: path)
    guard FileManager.default.fileExists(atPath: path) else { return nil }
    guard let fileHandle = try? FileHandle(forReadingFrom: url) else { return nil }
    defer {
      try? fileHandle.close()
    }
    do {
      guard let riffData = try fileHandle.read(upToCount: 12), riffData.count == 12 else {
        return nil
      }
      let riff = String(decoding: riffData.subdata(in: 0..<4), as: UTF8.self)
      let wave = String(decoding: riffData.subdata(in: 8..<12), as: UTF8.self)
      guard (riff == "RIFF" || riff == "RF64") && wave == "WAVE" else {
        return nil
      }
      try fileHandle.seek(toOffset: 12)
      guard let remainingData = try fileHandle.read(upToCount: 1024) else {
        return nil
      }
      let fmtMarker = Data("fmt ".utf8)
      guard let fmtOffset = remainingData.range(of: fmtMarker)?.lowerBound else {
        return nil
      }
      guard fmtOffset + 16 <= remainingData.count else {
        return nil
      }
      let numChannels = remainingData.subdata(in: (fmtOffset + 10)..<(fmtOffset + 12)).withUnsafeBytes { $0.load(as: UInt16.self) }
      let sampleRate = remainingData.subdata(in: (fmtOffset + 12)..<(fmtOffset + 16)).withUnsafeBytes { $0.load(as: UInt32.self) }
      guard numChannels > 0 && numChannels <= 32 else { return nil }
      guard sampleRate >= 8000 && sampleRate <= 768000 else { return nil }
      return (channels: Int(numChannels), sampleRate: Int(sampleRate))
    } catch {
      return nil
    }
  }

  public static func bestRate(from rates: [Int], preferring current: Int) -> Int {
    if rates.contains(current) { return current }
    for preferred in [48000, 44100, 96000, 192000] {
      if rates.contains(preferred) { return preferred }
    }
    return rates.min(by: { abs($0 - current) < abs($1 - current) }) ?? 48000
  }
}
