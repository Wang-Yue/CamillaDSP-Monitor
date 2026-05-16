// Standalone resampler configuration types.

import Foundation

public enum ResamplerType: String, Codable, Equatable, Sendable {
  case synchronous = "Synchronous"
  case apple = "Apple"
  case asyncSinc = "AsyncSinc"
  case asyncPoly = "AsyncPoly"
}

/// Quality settings supported by Apple's AudioConverter.
public enum AppleResamplerQuality: String, Codable, Sendable, CaseIterable, Identifiable, Equatable
{
  case min = "Min"
  case low = "Low"
  case medium = "Medium"
  case high = "High"
  case max = "Max"

  public var id: String { rawValue }
}

/// Algorithm complexity supported by Apple's AudioConverter.
public enum AppleResamplerComplexity: String, Codable, Sendable, CaseIterable, Identifiable,
  Equatable
{
  case linear = "Linear"
  case normal = "Normal"
  case mastering = "Mastering"
  case minimumPhase = "MinimumPhase"

  public var id: String { rawValue }

  public var osType: UInt32 {
    switch self {
    case .linear: return 0x6C69_6E65  // 'line'
    case .normal: return 0x6E6F_726D  // 'norm'
    case .mastering: return 0x6261_7473  // 'bats'
    case .minimumPhase: return 0x6D69_6E70  // 'minp'
    }
  }
}

public struct ResamplerConfig: Codable, Equatable, Sendable {
  public var type: ResamplerType
  public var profile: String?
  public var interpolation: String?
  public var appleQuality: AppleResamplerQuality?
  public var appleComplexity: AppleResamplerComplexity?

  enum CodingKeys: String, CodingKey {
    case type
    case profile
    case interpolation
    case appleQuality = "apple_quality"
    case appleComplexity = "apple_complexity"
  }

  public init(
    type: ResamplerType,
    profile: String? = nil,
    interpolation: String? = nil,
    appleQuality: AppleResamplerQuality? = nil,
    appleComplexity: AppleResamplerComplexity? = nil
  ) {
    self.type = type
    self.profile = profile
    self.interpolation = interpolation
    self.appleQuality = appleQuality
    self.appleComplexity = appleComplexity
  }
}
