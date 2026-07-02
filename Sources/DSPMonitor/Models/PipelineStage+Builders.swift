// PipelineStage+Builders - Build DSP config components using Dictionaries

import DSPConfig
import Foundation

extension PipelineStage {

  func buildFilters(
    eqPresets: [EQPreset],
    convPresets: [ConvolutionPreset],
    sampleRate: Int
  ) -> [String: FilterConfig] {
    guard isActive else { return [:] }
    let prefix = "\(type.id.lowercased())_\(id.uuidString.prefix(8))"

    switch type {
    case .balance, .width, .msProc, .mixer, .compressor, .noiseGate, .race:
      return [:]

    case .phaseInvert:
      return ["\(prefix)_invert": .gain(GainParameters(gain: 0.0, inverted: true))]

    case .crossfeed:
      let cx = activeCrossfeedParams
      return [
        "\(prefix)_hi": .biquad(
          BiquadParameters(type: .lowshelf, freq: cx.hiFreq, gain: cx.hiGain, q: cx.hiQ)),
        "\(prefix)_lo": .biquad(BiquadParameters(type: .lowpassFO, freq: cx.loFreq)),
        "\(prefix)_lo_gain": .gain(GainParameters(gain: cx.loGain, inverted: false)),
      ]

    case .eq:
      guard let presetID = eqPresetID, let preset = eqPresets.first(where: { $0.id == presetID })
      else { return [:] }
      var filters: [String: FilterConfig] = [:]
      filters["\(prefix)_preamp"] = .gain(GainParameters(gain: preset.preampGain, inverted: false))

      for (i, band) in preset.bands.enumerated() where band.isEnabled {
        let biquadType = BiquadType(rawValue: band.type.rawValue) ?? .peaking
        var params = BiquadParameters(type: biquadType)

        switch band.type {
        case .free:
          params.b0 = band.b0
          params.b1 = band.b1
          params.b2 = band.b2
          params.a1 = band.a1
          params.a2 = band.a2
        case .generalNotch:
          params.freqNotch = band.freqNotch
          params.freqPole = band.freqPole
          params.qP = band.qPole
          params.normalizeAtDc = band.normalizeAtDc
        case .linkwitzTransform:
          params.freqAct = band.freqAct
          params.qAct = band.qAct
          params.freqTarget = band.freqTarget
          params.qTarget = band.qTarget
        default:
          params.freq = band.freq
          params.gain = band.type.hasGain ? band.gain : nil
          params.q = band.type.hasQ ? band.q : nil
        }

        filters["\(prefix)_\(i + 1)"] = .biquad(params)
      }
      return filters

    case .convolution:
      guard let presetID = convPresetID,
        let preset = convPresets.first(where: { $0.id == presetID }),
        let path = preset.irPath(forSampleRate: sampleRate)
      else { return [:] }
      return ["\(prefix)_conv": .conv(ConvParameters(type: .raw, filename: path, format: "F64_LE"))]

    case .loudness:
      return [
        "\(prefix)_loudness": .loudness(
          LoudnessParameters(
            referenceLevel: loudnessReference,
            highBoost: loudnessHighBoost,
            lowBoost: loudnessLowBoost
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
          "\(prefix)_deemphasis": .biquad(
            BiquadParameters(type: subtype, freq: freq, gain: -9.5, q: q))
        ]
      case .preEmphasis:
        return [
          "\(prefix)_preemphasis": .biquad(
            BiquadParameters(type: subtype, freq: freq, gain: 9.5, q: q))
        ]
      }

    case .dcProtection:
      return ["\(prefix)_dcp": .biquad(BiquadParameters(type: .highpassFO, freq: 7.0))]

    case .gain:
      return [
        "\(prefix)_gain": .gain(
          GainParameters(gain: gainValue, scale: .dB, inverted: gainInverted, mute: gainMuted))
      ]

    case .delay:
      return ["\(prefix)_delay": .delay(DelayParameters(delay: delayValue, unit: delayUnit))]

    case .limiter:
      return [
        "\(prefix)_limiter": .lookaheadLimiter(
          LookaheadLimiterParameters(
            limit: limiterLimit, attack: limiterAttack, release: limiterRelease, unit: .ms))
      ]

    case .dither:
      return [
        "\(prefix)_dither": .dither(
          DitherParameters(type: ditherType, bits: ditherBits, amplitude: ditherAmplitude))
      ]

    case .diffEq:
      let aVals = diffEqA.components(separatedBy: ",").compactMap {
        Double($0.trimmingCharacters(in: .whitespaces))
      }
      let bVals = diffEqB.components(separatedBy: ",").compactMap {
        Double($0.trimmingCharacters(in: .whitespaces))
      }
      return [
        "\(prefix)_diffeq": .diffEq(
          DiffEqParameters(a: aVals.isEmpty ? nil : aVals, b: bVals.isEmpty ? nil : bVals))
      ]

    case .biquadCombo:
      var params = BiquadComboParameters(type: comboType)
      switch comboType {
      case .butterworthHighpass, .butterworthLowpass, .linkwitzRileyHighpass, .linkwitzRileyLowpass:
        params.freq = comboFreq
        params.order = comboOrder
      case .tilt:
        params.freq = comboFreq
        params.gain = comboGain
      default:
        break
      }
      return ["\(prefix)_combo": .biquadCombo(params)]

    case .clipper:
      return [
        "\(prefix)_clipper": .limiter(
          LimiterParameters(clipLimit: clipperLimit, softClip: clipperSoftClip))
      ]

    case .graphicEQ:
      var params = BiquadComboParameters(type: .graphicEqualizer)
      params.freqMin = graphicEQFreqMin
      params.freqMax = graphicEQFreqMax
      params.gains = graphicEQGains
      return ["\(prefix)_geq": .biquadCombo(params)]
    }
  }

  func buildMixers(channels: Int) -> [String: MixerConfig] {
    guard isActive else { return [:] }
    let prefix = "\(type.id.lowercased())_\(id.uuidString.prefix(8))"

    switch type {
    case .balance:
      let leftLin = 1.0 - max(0.0, balancePosition)
      let rightLin = 1.0 + min(0.0, balancePosition)
      let leftDB = leftLin > 0 ? 20.0 * log10(leftLin) : -100.0
      let rightDB = rightLin > 0 ? 20.0 * log10(rightLin) : -100.0

      var mapping: [MixerMapping] = []
      for i in 0..<channels {
        if i == leftChannel {
          mapping.append(
            MixerMapping(dest: i, sources: [MixerSource(channel: leftChannel, gain: leftDB)]))
        } else if i == rightChannel {
          mapping.append(
            MixerMapping(dest: i, sources: [MixerSource(channel: rightChannel, gain: rightDB)]))
        } else {
          mapping.append(MixerMapping(dest: i, sources: [MixerSource(channel: i, gain: 0.0)]))
        }
      }
      return [
        prefix: MixerConfig(
          channelsIn: channels,
          channelsOut: channels,
          mapping: mapping
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
          sources.append(
            MixerSource(channel: leftChannel, gain: 20.0 * log10(abs(ch0)), inverted: ch0 < 0))
        }
        if abs(ch1) > threshold {
          sources.append(
            MixerSource(channel: rightChannel, gain: 20.0 * log10(abs(ch1)), inverted: ch1 < 0))
        }
        return sources
      }

      var mapping: [MixerMapping] = []
      for i in 0..<channels {
        if i == leftChannel {
          mapping.append(MixerMapping(dest: i, sources: makeSources(ch0: ll, ch1: lr)))
        } else if i == rightChannel {
          mapping.append(MixerMapping(dest: i, sources: makeSources(ch0: lr, ch1: ll)))
        } else {
          mapping.append(MixerMapping(dest: i, sources: [MixerSource(channel: i, gain: 0.0)]))
        }
      }
      return [
        prefix: MixerConfig(
          channelsIn: channels,
          channelsOut: channels,
          mapping: mapping
        )
      ]

    case .msProc:
      var mapping: [MixerMapping] = []
      for i in 0..<channels {
        if i == leftChannel {
          mapping.append(
            MixerMapping(
              dest: i,
              sources: [
                MixerSource(channel: leftChannel, gain: -6.02),
                MixerSource(channel: rightChannel, gain: -6.02),
              ]))
        } else if i == rightChannel {
          mapping.append(
            MixerMapping(
              dest: i,
              sources: [
                MixerSource(channel: leftChannel, gain: -6.02),
                MixerSource(channel: rightChannel, gain: -6.02, inverted: true),
              ]))
        } else {
          mapping.append(MixerMapping(dest: i, sources: [MixerSource(channel: i, gain: 0.0)]))
        }
      }
      return [
        prefix: MixerConfig(
          channelsIn: channels,
          channelsOut: channels,
          mapping: mapping
        )
      ]

    case .crossfeed:
      guard crossfeedLevel != .off else { return [:] }

      var otherChannels: [Int] = []
      for i in 0..<channels {
        if i != leftChannel && i != rightChannel {
          otherChannels.append(i)
        }
      }

      var mapping2to4 = [
        MixerMapping(dest: 0, sources: [MixerSource(channel: leftChannel, gain: 0.0)]),
        MixerMapping(dest: 1, sources: [MixerSource(channel: leftChannel, gain: 0.0)]),
        MixerMapping(dest: 2, sources: [MixerSource(channel: rightChannel, gain: 0.0)]),
        MixerMapping(dest: 3, sources: [MixerSource(channel: rightChannel, gain: 0.0)]),
      ]
      for (idx, ch) in otherChannels.enumerated() {
        mapping2to4.append(
          MixerMapping(dest: idx + 4, sources: [MixerSource(channel: ch, gain: 0.0)]))
      }

      var mapping4to2: [MixerMapping] = Array(
        repeating: MixerMapping(dest: 0, sources: []), count: channels)
      mapping4to2[leftChannel] = MixerMapping(
        dest: leftChannel,
        sources: [
          MixerSource(channel: 0, gain: 0.0),
          MixerSource(channel: 2, gain: 0.0),
        ])
      mapping4to2[rightChannel] = MixerMapping(
        dest: rightChannel,
        sources: [
          MixerSource(channel: 1, gain: 0.0),
          MixerSource(channel: 3, gain: 0.0),
        ])
      for (idx, ch) in otherChannels.enumerated() {
        mapping4to2[ch] = MixerMapping(
          dest: idx + 4, sources: [MixerSource(channel: idx + 4, gain: 0.0)])
      }

      return [
        "\(prefix)_2to4": MixerConfig(
          channelsIn: channels,
          channelsOut: channels + 2,
          mapping: mapping2to4
        ),
        "\(prefix)_4to2": MixerConfig(
          channelsIn: channels + 2,
          channelsOut: channels,
          mapping: mapping4to2
        ),
      ]

    case .mixer:
      var mapping = mixerMappings
      if mapping.isEmpty {
        mapping = (0..<mixerChannelsOut).map { i in
          let src = i < mixerChannelsIn ? i : 0
          return MixerMapping(dest: i, sources: [MixerSource(channel: src, gain: 0.0)])
        }
      } else if mapping.count < mixerChannelsOut {
        for i in mapping.count..<mixerChannelsOut {
          let src = i < mixerChannelsIn ? i : 0
          mapping.append(MixerMapping(dest: i, sources: [MixerSource(channel: src, gain: 0.0)]))
        }
      } else if mapping.count > mixerChannelsOut {
        mapping = Array(mapping.prefix(mixerChannelsOut))
      }

      let cleanedMapping = mapping.map { map in
        let cleanedSources = map.sources.filter { $0.channel < mixerChannelsIn }
        return MixerMapping(dest: map.dest, sources: cleanedSources)
      }

      return [
        prefix: MixerConfig(
          channelsIn: mixerChannelsIn,
          channelsOut: mixerChannelsOut,
          mapping: cleanedMapping
        )
      ]

    case .compressor, .noiseGate, .race:
      return [:]
    default:
      return [:]
    }
  }

  func buildProcessors(channels: Int) -> [String: ProcessorConfig] {
    guard isActive else { return [:] }
    let prefix = "\(type.id.lowercased())_\(id.uuidString.prefix(8))"
    let chList = self.channels.sorted()

    switch type {
    case .compressor:
      let params = CompressorParameters(
        channels: channels,
        monitorChannels: chList,
        processChannels: chList,
        attack: compressorAttack,
        release: compressorRelease,
        threshold: compressorThreshold,
        factor: compressorRatio,
        makeupGain: compressorMakeupGain,
        softClip: compressorSoftClip,
        clipLimit: compressorClipLimit
      )
      return [prefix: .compressor(params)]

    case .noiseGate:
      let params = NoiseGateParameters(
        channels: channels,
        monitorChannels: chList,
        processChannels: chList,
        attack: gateAttack,
        release: gateRelease,
        threshold: gateThreshold,
        attenuation: gateAttenuation
      )
      return [prefix: .noiseGate(params)]

    case .race:
      let params = RACEParameters(
        channels: channels,
        channelA: leftChannel,
        channelB: rightChannel,
        delay: raceDelay,
        subsampleDelay: false,
        delayUnit: .ms,
        attenuation: raceAttenuation
      )
      return [prefix: .race(params)]

    default:
      return [:]
    }
  }

  func buildPipelineSteps(
    eqPresets: [EQPreset],
    convPresets: [ConvolutionPreset],
    sampleRate: Int
  ) -> [PipelineStep] {
    guard isActive else { return [] }
    let prefix = "\(type.id.lowercased())_\(id.uuidString.prefix(8))"
    let chList = self.channels.sorted()

    switch type {
    case .balance, .width, .msProc, .mixer:
      return [PipelineStep(type: .mixer, name: prefix)]

    case .phaseInvert:
      guard !channels.isEmpty else { return [] }
      return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_invert"])]

    case .crossfeed:
      guard crossfeedLevel != .off else { return [] }
      return [
        PipelineStep(type: .mixer, name: "\(prefix)_2to4"),
        PipelineStep(type: .filter, channels: [0, 3], names: ["\(prefix)_hi"]),
        PipelineStep(
          type: .filter, channels: [1, 2], names: ["\(prefix)_lo", "\(prefix)_lo_gain"]),
        PipelineStep(type: .mixer, name: "\(prefix)_4to2"),
      ]

    case .eq:
      guard let presetID = eqPresetID, let preset = eqPresets.first(where: { $0.id == presetID }),
        !channels.isEmpty
      else { return [] }
      var names = ["\(prefix)_preamp"]
      names.append(
        contentsOf: preset.bands.enumerated().compactMap { i, b in
          b.isEnabled ? "\(prefix)_\(i + 1)" : nil
        })
      return [PipelineStep(type: .filter, channels: chList, names: names)]

    case .convolution:
      guard let presetID = convPresetID,
        let preset = convPresets.first(where: { $0.id == presetID }),
        preset.irPath(forSampleRate: sampleRate) != nil, !channels.isEmpty
      else { return [] }
      return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_conv"])]

    case .loudness:
      guard !channels.isEmpty else { return [] }
      return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_loudness"])]

    case .emphasis:
      guard !channels.isEmpty else { return [] }
      switch emphasisMode {
      case .off: return []
      case .deEmphasis:
        return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_deemphasis"])]
      case .preEmphasis:
        return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_preemphasis"])]
      }

    case .dcProtection:
      guard !channels.isEmpty else { return [] }
      return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_dcp"])]

    case .gain:
      guard !channels.isEmpty else { return [] }
      return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_gain"])]

    case .delay:
      guard !channels.isEmpty else { return [] }
      return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_delay"])]

    case .limiter:
      guard !channels.isEmpty else { return [] }
      return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_limiter"])]

    case .compressor, .noiseGate, .race:
      return [PipelineStep(type: .processor, name: prefix)]

    case .dither:
      guard !channels.isEmpty else { return [] }
      return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_dither"])]

    case .diffEq:
      guard !channels.isEmpty else { return [] }
      return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_diffeq"])]

    case .biquadCombo:
      guard !channels.isEmpty else { return [] }
      return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_combo"])]

    case .clipper:
      guard !channels.isEmpty else { return [] }
      return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_clipper"])]

    case .graphicEQ:
      guard !channels.isEmpty else { return [] }
      return [PipelineStep(type: .filter, channels: chList, names: ["\(prefix)_geq"])]
    }
  }
}

// MARK: - BiquadType Helpers

extension BiquadType {
  var hasGain: Bool {
    switch self {
    case .peaking, .lowshelf, .highshelf, .lowshelfFO, .highshelfFO: return true
    default: return false
    }
  }
  var hasQ: Bool {
    switch self {
    case .peaking, .lowpass, .highpass, .lowshelf, .highshelf, .notch, .bandpass, .allpass,
      .generalNotch, .linkwitzTransform:
      return true
    default: return false
    }
  }
}
