import DSPConfig
import DSPLib
import Foundation
import Observation
import SwiftDSP

@MainActor
@Observable
final class RoomCorrectionEngineController {
  let engine = DSPEngine()
  private weak var session: MeasurementSession?

  init(session: MeasurementSession) {
    self.session = session
  }

  func applyConfig() {
    guard let session = session, let preset = session.correctionPreset else { return }

    let rate = session.sampleRate

    // Default loopback capture for macOS is usually BlackHole 2ch
    let captureDevice = "BlackHole 2ch"
    let playbackDevice = session.selectedOutputName

    let captureConfig = CaptureDeviceConfig(
      type: .coreAudio,
      channels: 2,
      device: captureDevice
    )

    let playbackConfig = PlaybackDeviceConfig(
      type: .coreAudio,
      channels: 2,
      device: playbackDevice,
      exclusive: false
    )

    let devicesConfig = DevicesConfig(
      samplerate: rate,
      chunksize: 1024,
      capture: captureConfig,
      playback: playbackConfig
    )

    // Build the filters dictionary from the correction preset bands
    var filters: [String: FilterConfig] = [:]
    filters["preamp"] = .gain(GainParameters(gain: preset.preampGain, inverted: false))

    var filterNames = ["preamp"]
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
      case .lowshelf, .highshelf:
        params.freq = band.freq
        params.gain = band.gain
        if band.useSlope {
          params.slope = band.slope
        } else {
          params.q = band.q
        }
      case .notch, .bandpass, .allpass:
        params.freq = band.freq
        if band.useBandwidth {
          params.bandwidth = band.bandwidth
        } else {
          params.q = band.q
        }
      default:
        params.freq = band.freq
        params.gain = band.type.hasGain ? band.gain : nil
        params.q = band.type.hasQ ? band.q : nil
      }

      let filterName = "filter_\(i + 1)"
      filters[filterName] = .biquad(params)
      filterNames.append(filterName)
    }

    // Build pipeline steps: apply to channel 0 and 1 (left and right)
    let pipeline = [
      PipelineStep(type: .filter, channels: [0, 1], names: filterNames)
    ]

    var config = DSPConfiguration(devices: devicesConfig)
    config.filters = filters
    config.pipeline = pipeline

    do {
      let encoder = JSONEncoder()
      let data = try encoder.encode(config)
      if let json = String(data: data, encoding: .utf8) {
        Task {
          try? await engine.start(configJson: json)
        }
      }
    } catch {
      print("[RoomCorrectionEngineController] Failed to build config: \(error)")
    }
  }

}
