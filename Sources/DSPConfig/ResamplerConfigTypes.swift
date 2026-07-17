// Standalone resampler configuration types.

import Foundation

public enum ResamplerType: String, Codable, Equatable, Sendable {
  case synchronous = "Synchronous"
  case asyncSinc = "AsyncSinc"
  case asyncPoly = "AsyncPoly"
}

public struct ResamplerConfig: Codable, Equatable, Sendable, CustomStringConvertible {
  public var type: ResamplerType
  public var profile: String?
  public var interpolation: String?
  public var sincLen: Int?
  public var oversamplingFactor: Int?
  public var window: String?
  public var fCutoff: Double?

  enum CodingKeys: String, CodingKey {
    case type
    case profile
    case interpolation
    case sincLen = "sinc_len"
    case oversamplingFactor = "oversampling_factor"
    case window
    case fCutoff = "f_cutoff"
  }

  public init(
    type: ResamplerType,
    profile: String? = nil,
    interpolation: String? = nil,
    sincLen: Int? = nil,
    oversamplingFactor: Int? = nil,
    window: String? = nil,
    fCutoff: Double? = nil
  ) {
    self.type = type
    self.profile = profile
    self.interpolation = interpolation
    self.sincLen = sincLen
    self.oversamplingFactor = oversamplingFactor
    self.window = window
    self.fCutoff = fCutoff
  }

  public var description: String {
    "ResamplerConfig(type: \(type), profile: \(profile ?? "nil"), interpolation: \(interpolation ?? "nil"), sincLen: \(sincLen ?? 0))"
  }
}

public enum ResamplerProfile: String, Codable, Sendable, CaseIterable, Identifiable {
  case veryFast = "VeryFast"
  case fast = "Fast"
  case balanced = "Balanced"
  case accurate = "Accurate"

  public var id: String { rawValue }
}
