// PipelineStage+Defaults - Default stages and persistence (Snapshot)

import DSPAudio
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
    var monitorChannels: [Int]
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
    var loudnessFader: Int
    var loudnessAttenuateMid: Bool

    // New stage parameters
    var gainValue: Double
    var gainInverted: Bool
    var gainMuted: Bool
    var volumeRampTime: Double
    var volumeLimit: Double
    var volumeFader: Int
    var delayValue: Double
    var delayUnit: String
    var delaySubsample: Bool
    var lookaheadLimit: Double
    var lookaheadAttack: Double
    var lookaheadRelease: Double

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
    var raceSubsampleDelay: Bool
    var raceDelayUnit: String

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

    // FivePointPeq parameters
    var peqFls: Double
    var peqGls: Double
    var peqQls: Double
    var peqF1: Double
    var peqG1: Double
    var peqQ1: Double
    var peqF2: Double
    var peqG2: Double
    var peqQ2: Double
    var peqF3: Double
    var peqG3: Double
    var peqQ3: Double
    var peqFhs: Double
    var peqGhs: Double
    var peqQhs: Double

    // Limiter parameters
    var limiterLimit: Double
    var limiterSoftClip: Bool

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
      monitorChannels: Array(monitorChannels).sorted(),
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
      loudnessFader: loudnessFader.rawValue,
      loudnessAttenuateMid: loudnessAttenuateMid,

      // New parameters
      gainValue: gainValue,
      gainInverted: gainInverted,
      gainMuted: gainMuted,
      volumeRampTime: volumeRampTime,
      volumeLimit: volumeLimit,
      volumeFader: volumeFader.rawValue,
      delayValue: delayValue,
      delayUnit: delayUnit.rawValue,
      delaySubsample: delaySubsample,
      lookaheadLimit: lookaheadLimit,
      lookaheadAttack: lookaheadAttack,
      lookaheadRelease: lookaheadRelease,

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
      raceSubsampleDelay: raceSubsampleDelay,
      raceDelayUnit: raceDelayUnit.rawValue,

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

      // FivePointPeq
      peqFls: peqFls,
      peqGls: peqGls,
      peqQls: peqQls,
      peqF1: peqF1,
      peqG1: peqG1,
      peqQ1: peqQ1,
      peqF2: peqF2,
      peqG2: peqG2,
      peqQ2: peqQ2,
      peqF3: peqF3,
      peqG3: peqG3,
      peqQ3: peqQ3,
      peqFhs: peqFhs,
      peqGhs: peqGhs,
      peqQhs: peqQhs,

      // Limiter parameters
      limiterLimit: limiterLimit,
      limiterSoftClip: limiterSoftClip,

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
    monitorChannels = Set(s.monitorChannels)
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
    if let v = Fader(rawValue: s.loudnessFader) { loudnessFader = v }
    loudnessAttenuateMid = s.loudnessAttenuateMid

    // New parameters
    gainValue = s.gainValue
    gainInverted = s.gainInverted
    gainMuted = s.gainMuted
    volumeRampTime = s.volumeRampTime
    volumeLimit = s.volumeLimit
    if let v = Fader(rawValue: s.volumeFader) { volumeFader = v }
    delayValue = s.delayValue
    if let v = DelayUnit(rawValue: s.delayUnit) { delayUnit = v }
    delaySubsample = s.delaySubsample
    lookaheadLimit = s.lookaheadLimit
    lookaheadAttack = s.lookaheadAttack
    lookaheadRelease = s.lookaheadRelease

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
    raceSubsampleDelay = s.raceSubsampleDelay
    if let v = DelayUnit(rawValue: s.raceDelayUnit) { raceDelayUnit = v }

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

    // FivePointPeq
    peqFls = s.peqFls
    peqGls = s.peqGls
    peqQls = s.peqQls
    peqF1 = s.peqF1
    peqG1 = s.peqG1
    peqQ1 = s.peqQ1
    peqF2 = s.peqF2
    peqG2 = s.peqG2
    peqQ2 = s.peqQ2
    peqF3 = s.peqF3
    peqG3 = s.peqG3
    peqQ3 = s.peqQ3
    peqFhs = s.peqFhs
    peqGhs = s.peqGhs
    peqQhs = s.peqQhs

    // Limiter parameters
    limiterLimit = s.limiterLimit
    limiterSoftClip = s.limiterSoftClip

    // Graphic EQ
    graphicEQFreqMin = s.graphicEQFreqMin
    graphicEQFreqMax = s.graphicEQFreqMax
    graphicEQBandCount = s.graphicEQBandCount
    graphicEQGains = s.graphicEQGains
  }
}
