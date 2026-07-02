// PipelineStage+Crossfeed - Crossfeed filter definitions and computation

import Foundation

struct CrossfeedPresetValue: Sendable {
  let fc: Double
  let db: Double
}

extension PipelineStage {

  static let crossfeedPresets: [CrossfeedLevel: CrossfeedPresetValue] = [
    .l1: CrossfeedPresetValue(fc: 650, db: 13.5),
    .l2: CrossfeedPresetValue(fc: 650, db: 9.5),
    .l3: CrossfeedPresetValue(fc: 700, db: 6.0),
    .l4: CrossfeedPresetValue(fc: 700, db: 4.5),
    .l5: CrossfeedPresetValue(fc: 700, db: 3.0),
  ]

  static func computeCrossfeed(fc: Double, db: Double) -> (
    hiFreq: Double, hiGain: Double, hiQ: Double, loFreq: Double, loGain: Double
  ) {
    let gd = -5.0 * db / 6.0 - 3.0
    let adH = db / 6.0 - 3.0
    let aH = pow(10.0, adH / 20.0)
    let gH = 1.0 - aH
    let gdH = 20.0 * log10(max(gH, 1e-10))
    let fcH = fc * pow(2.0, (gd - gdH) / 12.0) / pow(10.0, -adH / 80.0 / 0.5)
    return (hiFreq: fcH, hiGain: adH, hiQ: 0.5, loFreq: fc, loGain: gd)
  }

  var activeCrossfeedParams:
    (hiFreq: Double, hiGain: Double, hiQ: Double, loFreq: Double, loGain: Double)
  {
    if cxCustomEnabled { return Self.computeCrossfeed(fc: cxFc, db: cxDb) }
    let p = Self.crossfeedPresets[crossfeedLevel] ?? CrossfeedPresetValue(fc: 700, db: 6.0)
    return Self.computeCrossfeed(fc: p.fc, db: p.db)
  }
}
