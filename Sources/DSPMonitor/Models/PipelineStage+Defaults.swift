// PipelineStage+Defaults - Default stages and persistence (Snapshot)

import DSPConfig
import Foundation

extension PipelineStage {

  // MARK: - Default stages (fixed set to start with)

  public static func defaultStages() -> [PipelineStage] {
    return [
      PipelineStage(type: .dcProtection, isEnabled: true),
      PipelineStage(type: .eq, isEnabled: false),
      PipelineStage(type: .loudness, isEnabled: false),
    ]
  }

  // MARK: - Persistence

  /// Codable snapshot of all mutable stage state
  public struct Snapshot: Codable {
    let id: UUID
    let stageType: String
    var name: String
    var isEnabled: Bool
    var channels: [Int]
    var leftChannel: Int
    var rightChannel: Int
    var balancePosition: Double
    var widthAmount: Double
    var crossfeedLevel: String
    var eqPresetID: String?
    var convPresetID: String?
    var emphasisMode: String
    var cxCustomEnabled: Bool
    var cxFc: Double
    var cxDb: Double
    var loudnessReference: Double
    var loudnessHighBoost: Double
    var loudnessLowBoost: Double

    // New stage parameters
    var gainValue: Double
    var gainInverted: Bool
    var gainMuted: Bool
    var delayValue: Double
    var delayUnit: String
    var limiterLimit: Double
    var limiterAttack: Double
    var limiterRelease: Double

    // Matrix Mixer channel layouts
    var mixerChannelsIn: Int
    var mixerChannelsOut: Int
    var mixerMappings: [MixerMapping]

    // Processor parameters
    var compressorAttack: Double
    var compressorRelease: Double
    var compressorThreshold: Double
    var compressorRatio: Double
    var compressorMakeupGain: Double
    var compressorSoftClip: Bool
    var compressorClipLimit: Double
    var gateAttack: Double
    var gateRelease: Double
    var gateThreshold: Double
    var gateAttenuation: Double
    var raceDelay: Double
    var raceAttenuation: Double

    // Dither parameters
    var ditherType: String
    var ditherBits: Int
    var ditherAmplitude: Double

    // DiffEq parameters
    var diffEqA: String
    var diffEqB: String

    // Biquad Combo parameters
    var comboType: String
    var comboFreq: Double
    var comboOrder: Int
    var comboGain: Double
    var comboGains: String
    var comboFreqMin: Double
    var comboFreqMax: Double

    // Clipper parameters
    var clipperLimit: Double
    var clipperSoftClip: Bool

    // Graphic EQ parameters
    var graphicEQFreqMin: Double
    var graphicEQFreqMax: Double
    var graphicEQBandCount: Int
    var graphicEQGains: [Double]
  }

  public func toSnapshot() -> Snapshot {
    Snapshot(
      id: id,
      stageType: type.rawValue,
      name: name,
      isEnabled: isEnabled,
      channels: Array(channels).sorted(),
      leftChannel: leftChannel,
      rightChannel: rightChannel,
      balancePosition: balancePosition,
      widthAmount: widthAmount,
      crossfeedLevel: crossfeedLevel.rawValue,
      eqPresetID: eqPresetID?.uuidString,
      convPresetID: convPresetID?.uuidString,
      emphasisMode: emphasisMode.rawValue,
      cxCustomEnabled: cxCustomEnabled,
      cxFc: cxFc,
      cxDb: cxDb,
      loudnessReference: loudnessReference,
      loudnessHighBoost: loudnessHighBoost,
      loudnessLowBoost: loudnessLowBoost,

      // New parameters
      gainValue: gainValue,
      gainInverted: gainInverted,
      gainMuted: gainMuted,
      delayValue: delayValue,
      delayUnit: delayUnit.rawValue,
      limiterLimit: limiterLimit,
      limiterAttack: limiterAttack,
      limiterRelease: limiterRelease,

      // Matrix Mixer
      mixerChannelsIn: mixerChannelsIn,
      mixerChannelsOut: mixerChannelsOut,
      mixerMappings: mixerMappings,

      // Processor parameters
      compressorAttack: compressorAttack,
      compressorRelease: compressorRelease,
      compressorThreshold: compressorThreshold,
      compressorRatio: compressorRatio,
      compressorMakeupGain: compressorMakeupGain,
      compressorSoftClip: compressorSoftClip,
      compressorClipLimit: compressorClipLimit,
      gateAttack: gateAttack,
      gateRelease: gateRelease,
      gateThreshold: gateThreshold,
      gateAttenuation: gateAttenuation,
      raceDelay: raceDelay,
      raceAttenuation: raceAttenuation,

      // Dither parameters
      ditherType: ditherType.rawValue,
      ditherBits: ditherBits,
      ditherAmplitude: ditherAmplitude,

      // DiffEq parameters
      diffEqA: diffEqA,
      diffEqB: diffEqB,

      // Biquad Combo parameters
      comboType: comboType.rawValue,
      comboFreq: comboFreq,
      comboOrder: comboOrder,
      comboGain: comboGain,
      comboGains: comboGains,
      comboFreqMin: comboFreqMin,
      comboFreqMax: comboFreqMax,

      // Clipper parameters
      clipperLimit: clipperLimit,
      clipperSoftClip: clipperSoftClip,

      // Graphic EQ
      graphicEQFreqMin: graphicEQFreqMin,
      graphicEQFreqMax: graphicEQFreqMax,
      graphicEQBandCount: graphicEQBandCount,
      graphicEQGains: graphicEQGains
    )
  }

  public func restore(from s: Snapshot) {
    name = s.name
    isEnabled = s.isEnabled
    channels = Set(s.channels)
    leftChannel = s.leftChannel
    rightChannel = s.rightChannel
    balancePosition = s.balancePosition
    widthAmount = s.widthAmount
    if let v = CrossfeedLevel(rawValue: s.crossfeedLevel) { crossfeedLevel = v }
    eqPresetID = s.eqPresetID.flatMap { UUID(uuidString: $0) }
    convPresetID = s.convPresetID.flatMap { UUID(uuidString: $0) }
    if let v = EmphasisMode(rawValue: s.emphasisMode) { emphasisMode = v }
    cxCustomEnabled = s.cxCustomEnabled
    cxFc = s.cxFc
    cxDb = s.cxDb
    loudnessReference = s.loudnessReference
    loudnessHighBoost = s.loudnessHighBoost
    loudnessLowBoost = s.loudnessLowBoost

    // New parameters
    gainValue = s.gainValue
    gainInverted = s.gainInverted
    gainMuted = s.gainMuted
    delayValue = s.delayValue
    if let v = DelayUnit(rawValue: s.delayUnit) { delayUnit = v }
    limiterLimit = s.limiterLimit
    limiterAttack = s.limiterAttack
    limiterRelease = s.limiterRelease

    // Matrix Mixer
    mixerChannelsIn = s.mixerChannelsIn
    mixerChannelsOut = s.mixerChannelsOut
    mixerMappings = s.mixerMappings

    // Processor parameters
    compressorAttack = s.compressorAttack
    compressorRelease = s.compressorRelease
    compressorThreshold = s.compressorThreshold
    compressorRatio = s.compressorRatio
    compressorMakeupGain = s.compressorMakeupGain
    compressorSoftClip = s.compressorSoftClip
    compressorClipLimit = s.compressorClipLimit
    gateAttack = s.gateAttack
    gateRelease = s.gateRelease
    gateThreshold = s.gateThreshold
    gateAttenuation = s.gateAttenuation
    raceDelay = s.raceDelay
    raceAttenuation = s.raceAttenuation

    // Dither parameters
    if let v = DitherType(rawValue: s.ditherType) { ditherType = v }
    ditherBits = s.ditherBits
    ditherAmplitude = s.ditherAmplitude

    // DiffEq parameters
    diffEqA = s.diffEqA
    diffEqB = s.diffEqB

    // Biquad Combo parameters
    if let v = BiquadComboType(rawValue: s.comboType) { comboType = v }
    comboFreq = s.comboFreq
    comboOrder = s.comboOrder
    comboGain = s.comboGain
    comboGains = s.comboGains
    comboFreqMin = s.comboFreqMin
    comboFreqMax = s.comboFreqMax

    // Clipper parameters
    clipperLimit = s.clipperLimit
    clipperSoftClip = s.clipperSoftClip

    // Graphic EQ
    graphicEQFreqMin = s.graphicEQFreqMin
    graphicEQFreqMax = s.graphicEQFreqMax
    graphicEQBandCount = s.graphicEQBandCount
    graphicEQGains = s.graphicEQGains
  }
}
