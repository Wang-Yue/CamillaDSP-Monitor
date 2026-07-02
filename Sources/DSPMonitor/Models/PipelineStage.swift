// PipelineStage - Dynamic, reorderable DSP pipeline stages

import DSPAudio
import DSPConfig
import Foundation
import Observation

enum StageType: String, CaseIterable, Codable, Identifiable {
  case balance = "Balance"
  case width = "Width"
  case msProc = "M/S Proc"
  case phaseInvert = "Phase Invert"
  case crossfeed = "Crossfeed"
  case eq = "EQ"
  case graphicEQ = "Graphic EQ"
  case convolution = "Convolution"
  case loudness = "Loudness"
  case emphasis = "Emphasis"
  case dcProtection = "DC Protection"
  case gain = "Gain"
  case delay = "Delay"
  case limiter = "Limiter"
  case volume = "Volume"
  case mixer = "Matrix Mixer"
  case compressor = "Compressor"
  case noiseGate = "Noise Gate"
  case race = "RACE"
  case dither = "Dither"
  case diffEq = "Differential Equation"
  case biquadCombo = "Biquad Combo"
  case clipper = "Clipper"

  var id: String { rawValue }
  var icon: String {
    switch self {
    case .balance: return "dial.low"
    case .width: return "arrow.left.and.right"
    case .msProc: return "waveform.path"
    case .phaseInvert: return "waveform.path.ecg"
    case .crossfeed: return "headphones"
    case .eq: return "slider.horizontal.3"
    case .graphicEQ: return "slider.vertical.3"
    case .convolution: return "waveform.badge.magnifyingglass"
    case .loudness: return "ear"
    case .emphasis: return "waveform"
    case .dcProtection: return "bolt.shield"
    case .gain: return "plus.minus"
    case .volume: return "speaker.wave.3"
    case .delay: return "clock"
    case .limiter: return "square.slash"
    case .mixer: return "grid"
    case .compressor: return "arrow.up.right.and.arrow.down.left.rectangle"
    case .noiseGate: return "waveform.badge.minus"
    case .race: return "speaker.wave.2.bubble"
    case .dither: return "square.grid.3x1.below.line.grid.1x2"
    case .diffEq: return "function"
    case .biquadCombo: return "arrow.up.and.down.and.arrow.left.and.right"
    case .clipper: return "scissors"
    }
  }
}

enum CrossfeedLevel: String, CaseIterable, Identifiable, Codable, Sendable {
  case off = "Off"
  case l1 = "L1"
  case l2 = "L2"
  case l3 = "L3"
  case l4 = "L4"
  case l5 = "L5"
  var id: String { rawValue }
  var description: String {
    switch self {
    case .off: ""
    case .l1: "Just a touch"
    case .l2: "Jan Meier"
    case .l3: "Chu Moy"
    case .l4: "30° 3m"
    case .l5: "Strong"
    }
  }
}

enum EmphasisMode: String, CaseIterable, Identifiable, Codable, Sendable {
  case off = "Off"
  case deEmphasis = "De-Emphasis"
  case preEmphasis = "Pre-Emphasis"
  var id: String { rawValue }
  var description: String {
    switch self {
    case .off: ""
    case .deEmphasis: "Highshelf at 5200 Hz, -9.5 dB, Q 0.5 (undo pre-emphasis)"
    case .preEmphasis: "Highshelf at 5200 Hz, +9.5 dB, Q 0.5 (boost highs)"
    }
  }
}

@Observable
final class PipelineStage: Identifiable, Hashable {
  let id: UUID
  let type: StageType
  var name: String
  var isEnabled: Bool

  // Dynamic channel mapping
  var channels: Set<Int> = [0, 1]

  // Stereo-specific channel routing (for Balance, Width, M/S, Crossfeed, RACE)
  var leftChannel: Int = 0
  var rightChannel: Int = 1

  // Stage-specific parameters
  var balancePosition: Double = 0.0
  var widthAmount: Double = 1.0
  var crossfeedLevel: CrossfeedLevel = .l1
  var cxCustomEnabled: Bool = false
  var cxFc: Double = 650.0
  var cxDb: Double = 13.5

  var eqPresetID: UUID?
  var convPresetID: UUID?

  var emphasisMode: EmphasisMode = .deEmphasis
  var loudnessReference: Double = -25.0
  var loudnessHighBoost: Double = 7.0
  var loudnessLowBoost: Double = 7.0
  var loudnessFader: Fader = .main
  var loudnessAttenuateMid: Bool = false

  // New stage parameters
  var gainValue: Double = 0.0
  var gainInverted: Bool = false
  var gainMuted: Bool = false

  var volumeRampTime: Double = 400.0
  var volumeLimit: Double = 10.0
  var volumeFader: Fader = .aux1

  var delayValue: Double = 0.0
  var delayUnit: DelayUnit = .ms
  var delaySubsample: Bool = false

  var limiterLimit: Double = 0.0
  var limiterAttack: Double = 5.0
  var limiterRelease: Double = 100.0

  // Matrix Mixer channel layouts
  var mixerChannelsIn: Int = 2
  var mixerChannelsOut: Int = 2
  var mixerMappings: [MixerMapping] = []

  // Compressor parameters
  var compressorAttack: Double = 5.0
  var compressorRelease: Double = 100.0
  var compressorThreshold: Double = -20.0
  var compressorRatio: Double = 2.0
  var compressorMakeupGain: Double = 0.0
  var compressorSoftClip: Bool = false
  var compressorClipLimit: Double = 0.0

  // Noise Gate parameters
  var gateAttack: Double = 5.0
  var gateRelease: Double = 100.0
  var gateThreshold: Double = -60.0
  var gateAttenuation: Double = -40.0

  // RACE parameters
  var raceDelay: Double = 0.25
  var raceAttenuation: Double = 6.0
  var raceSubsampleDelay: Bool = false
  var raceDelayUnit: DelayUnit = .ms

  // Dither parameters
  var ditherType: DitherType = .flat
  var ditherBits: Int = 16
  var ditherAmplitude: Double = 1.0

  // DiffEq parameters
  var diffEqA: String = "1.0, 0.5"
  var diffEqB: String = "0.5, 0.25"

  // Biquad Combo parameters
  var comboType: BiquadComboType = .butterworthLowpass
  var comboFreq: Double = 1000.0
  var comboOrder: Int = 2
  var comboGain: Double = 0.0
  var comboGains: String = "0.0, 0.0, 0.0, 0.0, 0.0"
  var comboFreqMin: Double = 20.0
  var comboFreqMax: Double = 20000.0

  // FivePointPeq parameters
  var peqFls: Double = 80.0
  var peqGls: Double = 0.0
  var peqQls: Double = 0.707
  var peqF1: Double = 200.0
  var peqG1: Double = 0.0
  var peqQ1: Double = 0.707
  var peqF2: Double = 1000.0
  var peqG2: Double = 0.0
  var peqQ2: Double = 0.707
  var peqF3: Double = 4000.0
  var peqG3: Double = 0.0
  var peqQ3: Double = 0.707
  var peqFhs: Double = 12000.0
  var peqGhs: Double = 0.0
  var peqQhs: Double = 0.707

  // Clipper (Simple Limiter) parameters
  var clipperLimit: Double = 0.0
  var clipperSoftClip: Bool = false

  // Graphic EQ parameters
  var graphicEQFreqMin: Double = 20.0
  var graphicEQFreqMax: Double = 20000.0
  var graphicEQBandCount: Int = 31 {
    didSet {
      if graphicEQGains.count != graphicEQBandCount {
        if graphicEQGains.count < graphicEQBandCount {
          graphicEQGains.append(
            contentsOf: Array(repeating: 0.0, count: graphicEQBandCount - graphicEQGains.count))
        } else {
          graphicEQGains = Array(graphicEQGains.prefix(graphicEQBandCount))
        }
      }
    }
  }
  var graphicEQGains: [Double] = Array(repeating: 0.0, count: 31)

  init(
    id: UUID = UUID(), type: StageType, name: String? = nil, isEnabled: Bool = false,
    channels: Set<Int> = [0, 1]
  ) {
    self.id = id
    self.type = type
    self.name = name ?? type.rawValue
    self.isEnabled = isEnabled
    self.channels = channels

    // Set default channels based on type
    if type == .balance || type == .width || type == .msProc || type == .crossfeed || type == .race
    {
      self.leftChannel = 0
      self.rightChannel = 1
    }

    // Initialize default mixer mappings (1:1 passthrough for 2 channels)
    if type == .mixer {
      self.mixerMappings = [
        MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)]),
        MixerMapping(dest: 1, sources: [MixerSource(channel: 1, gain: 0.0)]),
      ]
    }
  }

  var balanceLeftPercent: Int { Int((1.0 - max(0, balancePosition)) * 100) }
  var balanceRightPercent: Int { Int((1.0 + min(0, balancePosition)) * 100) }
  var widthPercent: Int { Int(widthAmount * 100) }

  var widthDescription: String {
    if widthAmount == 1.0 { return "Normal stereo (passthrough)" }
    if widthAmount == 0.0 { return "Mono — L and R summed equally" }
    if widthAmount == -1.0 { return "Fully swapped — L and R exchanged" }
    if widthAmount < 0 { return "Partially swapped with crossfeed" }
    if widthAmount < 1.0 { return "Narrowed stereo image" }
    return "Enhanced stereo — wider than original"
  }

  var isActive: Bool {
    guard isEnabled else { return false }
    switch type {
    case .width: return widthAmount != 1.0
    case .balance: return balancePosition != 0.0
    case .crossfeed: return crossfeedLevel != .off
    case .emphasis: return emphasisMode != .off
    case .convolution: return convPresetID != nil
    case .eq: return eqPresetID != nil
    default: return true
    }
  }

  // Hashable & Equatable
  func hash(into hasher: inout Hasher) {
    hasher.combine(id)
  }

  static func == (lhs: PipelineStage, rhs: PipelineStage) -> Bool {
    lhs.id == rhs.id
  }
}
