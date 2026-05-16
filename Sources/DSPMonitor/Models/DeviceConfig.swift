import DSPConfig
import Foundation

/// Combined device config: selected device name, channel/rate/format, and fetched capabilities.
/// `capabilities.name == ""` means system default (no specific device selected).
/// `enforced()` cascades any out-of-range selection down to the nearest valid value.
public struct DeviceConfig: Equatable, Sendable, Codable {
  /// Full capabilities as reported by the device. `name` field doubles as the selected device name;
  /// empty name means "system default". `capability_sets` may be empty before capabilities are fetched.
  public var capabilities: AudioDeviceDescriptor

  public var channels: Int
  public var sampleRate: Int
  public var format: String
  public var bypassDoP: Bool
  /// DoP decimator passband cutoff in Hz. 20 kHz keeps SINAD highest;
  /// 30–50 kHz widens the audible passband at modest SINAD cost. Ignored
  /// when `bypassDoP` is true.
  public var dopCutoffHz: Double

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
    self.capabilities = AudioDeviceDescriptor()
    self.channels = 2
    self.sampleRate = 48000
    self.format = "F32"
    self.bypassDoP = false
    self.dopCutoffHz = 20_000
  }

  // Custom decode tolerates configs persisted before `dopCutoffHz` / `outputDoP` existed.
  private enum CodingKeys: String, CodingKey {
    case capabilities, channels, sampleRate, format, bypassDoP, dopCutoffHz
  }

  public init(from decoder: Decoder) throws {
    let c = try decoder.container(keyedBy: CodingKeys.self)
    self.capabilities = try c.decode(AudioDeviceDescriptor.self, forKey: .capabilities)
    self.channels = try c.decode(Int.self, forKey: .channels)
    self.sampleRate = try c.decode(Int.self, forKey: .sampleRate)
    self.format = try c.decode(String.self, forKey: .format)
    self.bypassDoP = try c.decode(Bool.self, forKey: .bypassDoP)
    self.dopCutoffHz = try c.decodeIfPresent(Double.self, forKey: .dopCutoffHz) ?? 20_000

  }

  // MARK: - Capabilities Logic

  private static let formatPriority: [String: Int] = [
    "S32": 4, "S24": 3, "S16": 2, "F32": 1, "F64": 0,
  ]

  /// Channel counts this device supports, sorted ascending.
  public var supportedChannels: [Int] {
    capabilities.capability_sets.first?.capabilities.map { $0.channels }.sorted() ?? []
  }

  /// Supported sample rates for a given channel count.
  /// Falls back to the union across all channel counts if the count is not found.
  public var supportedRates: [Int] {
    guard let set = capabilities.capability_sets.first else { return [] }
    let cap = set.capabilities.first(where: { $0.channels == channels }) ?? set.capabilities.first
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
    let cap = set.capabilities.first(where: { $0.channels == channels }) ?? set.capabilities.first
    let formats = cap?.samplerates.first(where: { $0.samplerate == sampleRate })?.formats ?? []
    return formats.sorted { (Self.formatPriority[$0] ?? -1) > (Self.formatPriority[$1] ?? -1) }
  }

  /// Returns a copy with channels/rate/format snapped to supported values.
  /// Pure function - no side effects.
  public func enforced() -> DeviceConfig {
    var result = self
    let ch = result.supportedChannels
    if !ch.isEmpty && !ch.contains(result.channels) {
      result.channels = ch.contains(2) ? 2 : ch[0]
    }
    let rates = result.supportedRates
    if !rates.isEmpty && !rates.contains(result.sampleRate) {
      result.sampleRate = Self.bestRate(from: rates, preferring: result.sampleRate)
    }
    let fmts = result.supportedFormats
    if !fmts.isEmpty && !fmts.contains(result.format) {
      result.format = fmts.first ?? "F32"
    }
    return result
  }

  public static func bestRate(from rates: [Int], preferring current: Int) -> Int {
    if rates.contains(current) { return current }
    for preferred in [48000, 44100, 96000, 192000] {
      if rates.contains(preferred) { return preferred }
    }
    return rates.min(by: { abs($0 - current) < abs($1 - current) }) ?? 48000
  }
}
