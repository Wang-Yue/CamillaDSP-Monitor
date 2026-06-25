// Standalone resampler configuration types.

import Foundation

public enum ResamplerType: String, Codable, Equatable, Sendable {
  case synchronous = "Synchronous"
  case asyncSinc = "AsyncSinc"
  case asyncPoly = "AsyncPoly"
}

public struct ResamplerConfig: Codable, Equatable, Sendable {
  public var type: ResamplerType
  public var profile: String?
  public var interpolation: String?

  enum CodingKeys: String, CodingKey {
    case type, profile, interpolation
  }

  public init(
    type: ResamplerType,
    profile: String? = nil,
    interpolation: String? = nil
  ) {
    self.type = type
    self.profile = profile
    self.interpolation = interpolation
  }
}

extension ResamplerConfig: CustomStringConvertible {
  public var description: String {
    "ResamplerConfig(type: \(type), profile: \(profile ?? "nil"), interpolation: \(interpolation ?? "nil"))"
  }
}
