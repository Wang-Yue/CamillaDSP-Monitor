import CamillaDSPLib
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
        name: newName, description: "", capability_sets: [])
    }
  }

  public init() {
    self.capabilities = AudioDeviceDescriptor()
    self.channels = 2
    self.sampleRate = 48000
    self.format = "F32"
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

  /// Best sample format for a given channel count and sample rate.
  public func bestFormat(channels: Int, sampleRate: Int) -> String {
    supportedFormats.first ?? "F32"
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
