// PipelineStage+Builders - Build CamillaDSP config components using Dictionaries

import Foundation

extension PipelineStage {

  func buildFilters() -> [String: [String: Any]] {
    guard isActive else { return [:] }
    switch type {
    case .balance, .width, .msProc: return [:]
    case .phaseInvert:
      return ["invert": ["type": "Gain", "parameters": ["gain": 0.0, "inverted": true]]]
    case .crossfeed:
      let cx = activeCrossfeedParams
      return [
        "cx_hi": [
          "type": "Biquad",
          "parameters": ["type": "Lowshelf", "freq": cx.hiFreq, "gain": cx.hiGain, "q": cx.hiQ],
        ],
        "cx_lo": ["type": "Biquad", "parameters": ["type": "LowpassFO", "freq": cx.loFreq]],
        "cx_lo_gain": ["type": "Gain", "parameters": ["gain": cx.loGain, "inverted": false]],
      ]
    case .eq: return [:]
    case .loudness:
      return [
        "loudness": [
          "type": "Loudness",
          "parameters": [
            "reference_level": Float(loudnessReference), "high_boost": Float(loudnessHighBoost),
            "low_boost": Float(loudnessLowBoost), "fader": "Main",
          ],
        ]
      ]
    case .emphasis:
      let subtype = "Highshelf"
      let freq = 5200.0
      let q = 0.5
      switch emphasisMode {
      case .off: return [:]
      case .deEmphasis:
        return [
          "deemphasis": [
            "type": "Biquad", "parameters": ["type": subtype, "freq": freq, "gain": -9.5, "q": q],
          ]
        ]
      case .preEmphasis:
        return [
          "preemphasis": [
            "type": "Biquad", "parameters": ["type": subtype, "freq": freq, "gain": 9.5, "q": q],
          ]
        ]
      }
    case .dcProtection:
      return ["dcp": ["type": "Biquad", "parameters": ["type": "HighpassFO", "freq": 7.0]]]
    }
  }

  func buildMixers() -> [String: [String: Any]] {
    guard isActive else { return [:] }
    switch type {
    case .balance:
      let leftLin = 1.0 - max(0.0, balancePosition)
      let rightLin = 1.0 + min(0.0, balancePosition)
      let leftDB = leftLin > 0 ? 20.0 * log10(leftLin) : -100.0
      let rightDB = rightLin > 0 ? 20.0 * log10(rightLin) : -100.0
      return [
        "balance": [
          "channels": ["in": 2, "out": 2],
          "mapping": [
            ["dest": 0, "sources": [["channel": 0, "gain": leftDB]]],
            ["dest": 1, "sources": [["channel": 1, "gain": rightDB]]],
          ],
        ]
      ]
    case .width:
      let w = widthAmount
      let ll = (1.0 + w) / 2.0
      let lr = (1.0 - w) / 2.0
      let llDB = ll > 0 ? 20.0 * log10(ll) : -100.0
      let lrDB = abs(lr) > 0 ? 20.0 * log10(abs(lr)) : -100.0
      return [
        "width": [
          "channels": ["in": 2, "out": 2],
          "mapping": [
            [
              "dest": 0,
              "sources": [
                ["channel": 0, "gain": llDB], ["channel": 1, "gain": lrDB, "inverted": lr < 0],
              ],
            ],
            [
              "dest": 1,
              "sources": [
                ["channel": 0, "gain": lrDB, "inverted": lr < 0], ["channel": 1, "gain": llDB],
              ],
            ],
          ],
        ]
      ]
    case .msProc:
      return [
        "msproc": [
          "channels": ["in": 2, "out": 2],
          "mapping": [
            [
              "dest": 0,
              "sources": [["channel": 0, "gain": -6.02], ["channel": 1, "gain": -6.02]],
            ],
            [
              "dest": 1,
              "sources": [
                ["channel": 0, "gain": -6.02], ["channel": 1, "gain": -6.02, "inverted": true],
              ],
            ],
          ],
        ]
      ]
    case .crossfeed:
      guard crossfeedLevel != .off else { return [:] }
      return [
        "2to4": [
          "channels": ["in": 2, "out": 4],
          "mapping": [
            ["dest": 0, "sources": [["channel": 0, "gain": 0.0]]],
            ["dest": 1, "sources": [["channel": 0, "gain": 0.0]]],
            ["dest": 2, "sources": [["channel": 1, "gain": 0.0]]],
            ["dest": 3, "sources": [["channel": 1, "gain": 0.0]]],
          ],
        ],
        "4to2": [
          "channels": ["in": 4, "out": 2],
          "mapping": [
            ["dest": 0, "sources": [["channel": 0, "gain": 0.0], ["channel": 2, "gain": 0.0]]],
            ["dest": 1, "sources": [["channel": 1, "gain": 0.0], ["channel": 3, "gain": 0.0]]],
          ],
        ],
      ]
    default: return [:]
    }
  }

  func buildPipelineSteps() -> [[String: Any]] {
    guard isActive else { return [] }
    switch type {
    case .balance: return [["type": "Mixer", "name": "balance"]]
    case .width: return [["type": "Mixer", "name": "width"]]
    case .msProc: return [["type": "Mixer", "name": "msproc"]]
    case .phaseInvert:
      switch phaseInvertMode {
      case .off: return []
      case .left: return [["type": "Filter", "channels": [0], "names": ["invert"]]]
      case .right: return [["type": "Filter", "channels": [1], "names": ["invert"]]]
      case .both: return [["type": "Filter", "channels": [0, 1], "names": ["invert"]]]
      }
    case .crossfeed:
      guard crossfeedLevel != .off else { return [] }
      return [
        ["type": "Mixer", "name": "2to4"],
        ["type": "Filter", "channels": [0, 3], "names": ["cx_hi"]],
        ["type": "Filter", "channels": [1, 2], "names": ["cx_lo", "cx_lo_gain"]],
        ["type": "Mixer", "name": "4to2"],
      ]
    case .eq: return []
    case .loudness: return [["type": "Filter", "channels": [0, 1], "names": ["loudness"]]]
    case .emphasis:
      switch emphasisMode {
      case .off: return []
      case .deEmphasis: return [["type": "Filter", "channels": [0, 1], "names": ["deemphasis"]]]
      case .preEmphasis: return [["type": "Filter", "channels": [0, 1], "names": ["preemphasis"]]]
      }
    case .dcProtection: return [["type": "Filter", "channels": [0, 1], "names": ["dcp"]]]
    }
  }

  func buildEQFilters(presets: [EQPreset]) -> [String: [String: Any]] {
    guard isActive, type == .eq else { return [:] }
    var filters: [String: [String: Any]] = [:]
    func addPresetFilters(_ preset: EQPreset, prefix: String) {
      filters["\(prefix)_preamp"] = [
        "type": "Gain", "parameters": ["gain": preset.preampGain, "inverted": false],
      ]
      for (i, band) in preset.bands.enumerated() where band.isEnabled {
        var p: [String: Any] = ["type": band.type.rawValue, "freq": band.freq]
        if band.type.hasGain { p["gain"] = band.gain }
        if band.type.hasQ { p["q"] = band.q }
        filters["\(prefix)_\(i + 1)"] = ["type": "Biquad", "parameters": p]
      }
    }
    switch eqChannelMode {
    case .same:
      if let id = eqPresetID, let preset = presets.first(where: { $0.id == id }) {
        addPresetFilters(preset, prefix: "eq")
      }
    case .separate:
      if let id = eqLeftPresetID, let preset = presets.first(where: { $0.id == id }) {
        addPresetFilters(preset, prefix: "eq_l")
      }
      if let id = eqRightPresetID, let preset = presets.first(where: { $0.id == id }) {
        addPresetFilters(preset, prefix: "eq_r")
      }
    }
    return filters
  }

  func buildEQPipelineSteps(presets: [EQPreset]) -> [[String: Any]] {
    guard isActive, type == .eq else { return [] }
    var steps: [[String: Any]] = []
    switch eqChannelMode {
    case .same:
      if let id = eqPresetID, let preset = presets.first(where: { $0.id == id }) {
        var names = ["eq_preamp"]
        names.append(
          contentsOf: preset.bands.enumerated().compactMap { i, b in
            b.isEnabled ? "eq_\(i + 1)" : nil
          })
        steps.append(["type": "Filter", "channels": [0, 1], "names": names])
      }
    case .separate:
      if let id = eqLeftPresetID, let preset = presets.first(where: { $0.id == id }) {
        var names = ["eq_l_preamp"]
        names.append(
          contentsOf: preset.bands.enumerated().compactMap { i, b in
            b.isEnabled ? "eq_l_\(i + 1)" : nil
          })
        steps.append(["type": "Filter", "channels": [0], "names": names])
      }
      if let id = eqRightPresetID, let preset = presets.first(where: { $0.id == id }) {
        var names = ["eq_r_preamp"]
        names.append(
          contentsOf: preset.bands.enumerated().compactMap { i, b in
            b.isEnabled ? "eq_r_\(i + 1)" : nil
          })
        steps.append(["type": "Filter", "channels": [1], "names": names])
      }
    }
    return steps
  }
}
