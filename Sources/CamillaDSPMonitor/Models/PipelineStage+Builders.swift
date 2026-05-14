// PipelineStage+Builders - Build CamillaDSP config components using Dictionaries

import DSPConfig
import Foundation

extension PipelineStage {

  func buildFilters() -> [String: FilterConfig] {
    guard isActive else { return [:] }
    switch type {
    case .balance, .width, .msProc: return [:]
    case .phaseInvert:
      return ["invert": .gain(GainParameters(gain: 0.0, inverted: true))]
    case .crossfeed:
      let cx = activeCrossfeedParams
      return [
        "cx_hi": .biquad(
          BiquadParameters(type: .lowshelf, freq: cx.hiFreq, gain: cx.hiGain, q: cx.hiQ)),
        "cx_lo": .biquad(BiquadParameters(type: .lowpassFO, freq: cx.loFreq)),
        "cx_lo_gain": .gain(GainParameters(gain: cx.loGain, inverted: false)),
      ]
    case .eq: return [:]
    case .convolution: return [:]
    case .loudness:
      return [
        "loudness": .loudness(
          LoudnessParameters(
            referenceLevel: loudnessReference,
            highBoost: loudnessHighBoost,
            lowBoost: loudnessLowBoost,
          )
        )
      ]
    case .emphasis:
      let subtype = BiquadType.highshelf
      let freq = 5200.0
      let q = 0.5
      switch emphasisMode {
      case .off: return [:]
      case .deEmphasis:
        return [
          "deemphasis": .biquad(BiquadParameters(type: subtype, freq: freq, gain: -9.5, q: q))
        ]
      case .preEmphasis:
        return [
          "preemphasis": .biquad(BiquadParameters(type: subtype, freq: freq, gain: 9.5, q: q))
        ]
      }
    case .dcProtection:
      return ["dcp": .biquad(BiquadParameters(type: .highpassFO, freq: 7.0))]
    }
  }

  func buildMixers() -> [String: MixerConfig] {
    guard isActive else { return [:] }
    switch type {
    case .balance:
      let leftLin = 1.0 - max(0.0, balancePosition)
      let rightLin = 1.0 + min(0.0, balancePosition)
      let leftDB = leftLin > 0 ? 20.0 * log10(leftLin) : -100.0
      let rightDB = rightLin > 0 ? 20.0 * log10(rightLin) : -100.0
      return [
        "balance": MixerConfig(
          channelsIn: 2,
          channelsOut: 2,
          mapping: [
            MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: leftDB)]),
            MixerMapping(dest: 1, sources: [MixerSource(channel: 1, gain: rightDB)]),
          ]
        )
      ]
    case .width:
      let w = widthAmount
      let ll = (1.0 + w) / 2.0
      let lr = (1.0 - w) / 2.0
      let threshold = 1e-6

      func makeSources(ch0: Double, ch1: Double) -> [MixerSource] {
        var sources: [MixerSource] = []
        if abs(ch0) > threshold {
          sources.append(MixerSource(channel: 0, gain: 20.0 * log10(abs(ch0)), inverted: ch0 < 0))
        }
        if abs(ch1) > threshold {
          sources.append(MixerSource(channel: 1, gain: 20.0 * log10(abs(ch1)), inverted: ch1 < 0))
        }
        return sources
      }

      return [
        "width": MixerConfig(
          channelsIn: 2,
          channelsOut: 2,
          mapping: [
            MixerMapping(dest: 0, sources: makeSources(ch0: ll, ch1: lr)),
            MixerMapping(dest: 1, sources: makeSources(ch0: lr, ch1: ll)),
          ]
        )
      ]
    case .msProc:
      return [
        "msproc": MixerConfig(
          channelsIn: 2,
          channelsOut: 2,
          mapping: [
            MixerMapping(
              dest: 0,
              sources: [MixerSource(channel: 0, gain: -6.02), MixerSource(channel: 1, gain: -6.02)]),
            MixerMapping(
              dest: 1,
              sources: [
                MixerSource(channel: 0, gain: -6.02),
                MixerSource(channel: 1, gain: -6.02, inverted: true),
              ]),
          ]
        )
      ]
    case .crossfeed:
      guard crossfeedLevel != .off else { return [:] }
      return [
        "2to4": MixerConfig(
          channelsIn: 2,
          channelsOut: 4,
          mapping: [
            MixerMapping(dest: 0, sources: [MixerSource(channel: 0, gain: 0.0)]),
            MixerMapping(dest: 1, sources: [MixerSource(channel: 0, gain: 0.0)]),
            MixerMapping(dest: 2, sources: [MixerSource(channel: 1, gain: 0.0)]),
            MixerMapping(dest: 3, sources: [MixerSource(channel: 1, gain: 0.0)]),
          ]
        ),
        "4to2": MixerConfig(
          channelsIn: 4,
          channelsOut: 2,
          mapping: [
            MixerMapping(
              dest: 0,
              sources: [MixerSource(channel: 0, gain: 0.0), MixerSource(channel: 2, gain: 0.0)]),
            MixerMapping(
              dest: 1,
              sources: [MixerSource(channel: 1, gain: 0.0), MixerSource(channel: 3, gain: 0.0)]),
          ]
        ),
      ]
    default: return [:]
    }
  }

  func buildPipelineSteps() -> [PipelineStep] {
    guard isActive else { return [] }
    switch type {
    case .balance: return [PipelineStep(type: .mixer, name: "balance")]
    case .width: return [PipelineStep(type: .mixer, name: "width")]
    case .msProc: return [PipelineStep(type: .mixer, name: "msproc")]
    case .phaseInvert:
      switch phaseInvertMode {
      case .off: return []
      case .left: return [PipelineStep(type: .filter, channels: [0], names: ["invert"])]
      case .right: return [PipelineStep(type: .filter, channels: [1], names: ["invert"])]
      case .both: return [PipelineStep(type: .filter, channels: [0, 1], names: ["invert"])]
      }
    case .crossfeed:
      guard crossfeedLevel != .off else { return [] }
      return [
        PipelineStep(type: .mixer, name: "2to4"),
        PipelineStep(type: .filter, channels: [0, 3], names: ["cx_hi"]),
        PipelineStep(type: .filter, channels: [1, 2], names: ["cx_lo", "cx_lo_gain"]),
        PipelineStep(type: .mixer, name: "4to2"),
      ]
    case .eq: return []
    case .convolution: return []
    case .loudness: return [PipelineStep(type: .filter, channels: [0, 1], names: ["loudness"])]
    case .emphasis:
      switch emphasisMode {
      case .off: return []
      case .deEmphasis:
        return [PipelineStep(type: .filter, channels: [0, 1], names: ["deemphasis"])]
      case .preEmphasis:
        return [PipelineStep(type: .filter, channels: [0, 1], names: ["preemphasis"])]
      }
    case .dcProtection: return [PipelineStep(type: .filter, channels: [0, 1], names: ["dcp"])]
    }
  }

  func buildEQFilters(presets: [EQPreset]) -> [String: FilterConfig] {
    guard isActive, type == .eq else { return [:] }
    var filters: [String: FilterConfig] = [:]
    func addPresetFilters(_ preset: EQPreset, prefix: String) {
      filters["\(prefix)_preamp"] = .gain(GainParameters(gain: preset.preampGain, inverted: false))
      for (i, band) in preset.bands.enumerated() where band.isEnabled {
        let biquadType = BiquadType(rawValue: band.type.rawValue) ?? .peaking
        let gainVal = band.type.hasGain ? band.gain : nil
        let qVal = band.type.hasQ ? band.q : nil
        filters["\(prefix)_\(i + 1)"] = .biquad(
          BiquadParameters(type: biquadType, freq: band.freq, gain: gainVal, q: qVal)
        )
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

  // MARK: - Convolution stage

  /// Emit `Conv` filter definitions referencing each selected preset's
  /// IR file *for the live capture rate*. Each preset stores a
  /// per-rate IR family; we look up the entry matching `sampleRate`
  /// and fall back to the closest available.
  ///
  /// Format strings match the Rust upstream's `FileSampleFormat`
  /// Display impl (`camilladsp/src/config/mod.rs:38`): valid set is
  /// `F32_LE / F64_LE / S16_LE / S24_3_LE / S24_4_LJ_LE / S24_4_RJ_LE
  /// / S32_LE / TEXT`. The `Conv { ... }` and `Raw { ... }` structs
  /// are `deny_unknown_fields`, so unknown values get rejected at
  /// config decode.
  func buildConvFilters(
    presets: [ConvolutionPreset], sampleRate: Int
  ) -> [String: FilterConfig] {
    guard isActive, type == .convolution else { return [:] }
    var filters: [String: FilterConfig] = [:]
    func make(_ preset: ConvolutionPreset) -> FilterConfig? {
      guard let path = preset.irPath(forSampleRate: sampleRate) else { return nil }
      return .conv(ConvParameters(type: .raw, filename: path, format: "F64_LE"))
    }
    switch convChannelMode {
    case .same:
      if let id = convPresetID, let preset = presets.first(where: { $0.id == id }),
        let f = make(preset)
      {
        filters["conv"] = f
      }
    case .separate:
      if let id = convLeftPresetID, let preset = presets.first(where: { $0.id == id }),
        let f = make(preset)
      {
        filters["conv_l"] = f
      }
      if let id = convRightPresetID, let preset = presets.first(where: { $0.id == id }),
        let f = make(preset)
      {
        filters["conv_r"] = f
      }
    }
    return filters
  }

  /// Pipeline steps for the convolution stage. We only emit the step
  /// when the corresponding preset has an IR for the live rate —
  /// otherwise the engine would build a filter graph referencing a
  /// non-existent filter name and fail.
  func buildConvPipelineSteps(
    presets: [ConvolutionPreset], sampleRate: Int
  ) -> [PipelineStep] {
    guard isActive, type == .convolution else { return [] }
    var steps: [PipelineStep] = []
    func hasIR(for id: UUID?) -> Bool {
      guard let id, let preset = presets.first(where: { $0.id == id }) else { return false }
      return preset.irPath(forSampleRate: sampleRate) != nil
    }
    switch convChannelMode {
    case .same:
      if hasIR(for: convPresetID) {
        steps.append(PipelineStep(type: .filter, channels: [0, 1], names: ["conv"]))
      }
    case .separate:
      if hasIR(for: convLeftPresetID) {
        steps.append(PipelineStep(type: .filter, channels: [0], names: ["conv_l"]))
      }
      if hasIR(for: convRightPresetID) {
        steps.append(PipelineStep(type: .filter, channels: [1], names: ["conv_r"]))
      }
    }
    return steps
  }

  func buildEQPipelineSteps(presets: [EQPreset]) -> [PipelineStep] {
    guard isActive, type == .eq else { return [] }
    var steps: [PipelineStep] = []
    switch eqChannelMode {
    case .same:
      if let id = eqPresetID, let preset = presets.first(where: { $0.id == id }) {
        var names = ["eq_preamp"]
        names.append(
          contentsOf: preset.bands.enumerated().compactMap { i, b in
            b.isEnabled ? "eq_\(i + 1)" : nil
          })
        steps.append(PipelineStep(type: .filter, channels: [0, 1], names: names))
      }
    case .separate:
      if let id = eqLeftPresetID, let preset = presets.first(where: { $0.id == id }) {
        var names = ["eq_l_preamp"]
        names.append(
          contentsOf: preset.bands.enumerated().compactMap { i, b in
            b.isEnabled ? "eq_l_\(i + 1)" : nil
          })
        steps.append(PipelineStep(type: .filter, channels: [0], names: names))
      }
      if let id = eqRightPresetID, let preset = presets.first(where: { $0.id == id }) {
        var names = ["eq_r_preamp"]
        names.append(
          contentsOf: preset.bands.enumerated().compactMap { i, b in
            b.isEnabled ? "eq_r_\(i + 1)" : nil
          })
        steps.append(PipelineStep(type: .filter, channels: [1], names: names))
      }
    }
    return steps
  }
}
