// PipelineStage - Configurable DSP pipeline stages matching CamillaDSP-Monitor YAML files exactly

import Foundation
import Observation

enum StageType: String, CaseIterable, Codable, Identifiable {
  case balance = "Balance"
  case width = "Width"
  case msProc = "M/S Proc"
  case phaseInvert = "Phase Invert"
  case crossfeed = "Crossfeed"
  case eq = "EQ"
  case loudness = "Loudness"
  case emphasis = "Emphasis"
  case dcProtection = "DC Protection"
  var id: String { rawValue }
  var icon: String {
    switch self {
    case .balance: return "dial.low"
    case .width: return "arrow.left.and.right"
    case .msProc: return "waveform.path"
    case .phaseInvert: return "waveform.path.ecg"
    case .crossfeed: return "headphones"
    case .eq: return "slider.horizontal.3"
    case .loudness: return "ear"
    case .emphasis: return "waveform"
    case .dcProtection: return "bolt.shield"
    }
  }
}

enum PhaseInvertMode: String, CaseIterable, Identifiable {
  case off = "Off"
  case left = "Left"
  case right = "Right"
  case both = "Both"
  var id: String { rawValue }
  var description: String {
    switch self {
    case .off: ""
    case .left: "Invert left channel only"
    case .right: "Invert right channel only"
    case .both: "Invert both channels (polarity flip)"
    }
  }
}

enum CrossfeedLevel: String, CaseIterable, Identifiable {
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

enum EQChannelMode: String, CaseIterable, Identifiable {
  case same = "Same L/R"
  case separate = "Separate L/R"
  var id: String { rawValue }
}

enum EmphasisMode: String, CaseIterable, Identifiable {
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
final class PipelineStage: Identifiable {
  let id = UUID()
  let type: StageType
  var name: String { type.rawValue }
  var isEnabled: Bool
  var balancePosition: Double = 0.0
  var widthAmount: Double = 1.0
  var phaseInvertMode: PhaseInvertMode = .both
  var crossfeedLevel: CrossfeedLevel = .l1
  var eqChannelMode: EQChannelMode = .same
  var eqPresetID: UUID?
  var eqLeftPresetID: UUID?
  var eqRightPresetID: UUID?
  var emphasisMode: EmphasisMode = .deEmphasis
  var cxCustomEnabled: Bool = false
  var cxFc: Double = 650.0
  var cxDb: Double = 13.5
  var loudnessReference: Double = -25.0
  var loudnessHighBoost: Double = 7.0
  var loudnessLowBoost: Double = 7.0

  init(type: StageType, isEnabled: Bool = false) {
    self.type = type
    self.isEnabled = isEnabled
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
    case .phaseInvert: return phaseInvertMode != .off
    case .crossfeed: return crossfeedLevel != .off
    case .emphasis: return emphasisMode != .off
    default: return true
    }
  }
}
