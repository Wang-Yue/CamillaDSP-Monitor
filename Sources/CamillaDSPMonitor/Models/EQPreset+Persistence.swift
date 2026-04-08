// EQPreset+Persistence - AppState extension for EQ preset persistence

import Foundation

extension AppState {
  func saveEQPresets() {
    if let data = try? JSONEncoder().encode(eqPresets) {
      defaults.set(data, forKey: "eqPresets")
    }
  }

  func loadEQPresets() -> [EQPreset] {
    guard let data = defaults.data(forKey: "eqPresets"),
      let presets = try? JSONDecoder().decode([EQPreset].self, from: data)
    else { return [] }
    return presets
  }

  func addEQPreset(name: String = "New Preset", preamp: Double = -6.0, bands: [EQBand]? = nil) {
    let preset = EQPreset(
      name: name,
      preampGain: preamp,
      bands: bands ?? [
        EQBand(type: .peaking, freq: 100, gain: 0, q: 1.0),
        EQBand(type: .peaking, freq: 1000, gain: 0, q: 1.0),
        EQBand(type: .peaking, freq: 10000, gain: 0, q: 1.0),
      ])
    eqPresets.append(preset)
    saveEQPresets()
  }

  func deleteEQPreset(at index: Int) {
    guard eqPresets.indices.contains(index) else { return }
    eqPresets.remove(at: index)
    saveEQPresets()
  }

  /// Create default presets on first launch (headphone + room L + room R)
  func createDefaultEQPresetsIfNeeded() {
    guard eqPresets.isEmpty else { return }

    // Headphone EQ (10 bands)
    let headphone = EQPreset(
      name: "Headphone EQ",
      preampGain: -6.0,
      bands: [
        EQBand(type: .peaking, freq: 20, gain: 4.0, q: 1.1),
        EQBand(type: .peaking, freq: 97, gain: -2.4, q: 0.7),
        EQBand(type: .lowshelf, freq: 105, gain: 5.5, q: 0.71),
        EQBand(type: .peaking, freq: 215, gain: -1.8, q: 1.1),
        EQBand(type: .peaking, freq: 1300, gain: -1.4, q: 1.5),
        EQBand(type: .highshelf, freq: 2000, gain: 3.0, q: 0.71),
        EQBand(type: .peaking, freq: 2700, gain: -1.3, q: 3.0),
        EQBand(type: .peaking, freq: 3250, gain: -3.0, q: 2.7),
        EQBand(type: .peaking, freq: 5400, gain: -1.6, q: 3.0),
        EQBand(type: .highshelf, freq: 11000, gain: -3.0, q: 0.71),
      ])

    // Room EQ Left (17 bands)
    let roomLeft = EQPreset(
      name: "Room EQ (Left)",
      preampGain: 0.0,
      bands: [
        EQBand(type: .peaking, freq: 224.0, gain: 18.0, q: 2.9),
        EQBand(type: .peaking, freq: 120.0, gain: -16.2, q: 5.6),
        EQBand(type: .peaking, freq: 402.7, gain: -11.7, q: 4.19),
        EQBand(type: .peaking, freq: 629.0, gain: -8.6, q: 2.98),
        EQBand(type: .peaking, freq: 1698.0, gain: 4.7, q: 1.31),
        EQBand(type: .peaking, freq: 1328.0, gain: -7.6, q: 3.38),
        EQBand(type: .peaking, freq: 181.9, gain: -18.1, q: 17.95),
        EQBand(type: .peaking, freq: 298.7, gain: -11.1, q: 11.18),
        EQBand(type: .peaking, freq: 223.5, gain: -15.9, q: 14.65),
        EQBand(type: .peaking, freq: 102.5, gain: 6.5, q: 7.13),
        EQBand(type: .peaking, freq: 135.4, gain: -11.9, q: 13.75),
        EQBand(type: .peaking, freq: 2403.0, gain: -4.1, q: 5.01),
        EQBand(type: .peaking, freq: 3774.0, gain: -2.3, q: 3.25),
        EQBand(type: .peaking, freq: 78.93, gain: 4.5, q: 9.61),
        EQBand(type: .peaking, freq: 862.0, gain: -4.2, q: 8.67),
        EQBand(type: .peaking, freq: 67.8, gain: -5.8, q: 15.45),
        EQBand(type: .peaking, freq: 58.88, gain: -6.6, q: 38.64),
      ])

    // Room EQ Right (17 bands)
    let roomRight = EQPreset(
      name: "Room EQ (Right)",
      preampGain: 0.0,
      bands: [
        EQBand(type: .peaking, freq: 2153.0, gain: -14.7, q: 2.33),
        EQBand(type: .peaking, freq: 54.5, gain: 18.0, q: 6.63),
        EQBand(type: .peaking, freq: 3141.0, gain: 9.2, q: 2.3),
        EQBand(type: .peaking, freq: 76.6, gain: -17.6, q: 7.31),
        EQBand(type: .peaking, freq: 59.1, gain: -24.7, q: 17.15),
        EQBand(type: .peaking, freq: 1950.0, gain: 13.6, q: 5.01),
        EQBand(type: .peaking, freq: 88.6, gain: 15.2, q: 7.75),
        EQBand(type: .peaking, freq: 132.5, gain: -15.5, q: 13.16),
        EQBand(type: .peaking, freq: 67.2, gain: 16.3, q: 19.1),
        EQBand(type: .peaking, freq: 873.0, gain: -3.5, q: 2.72),
        EQBand(type: .peaking, freq: 182.0, gain: -10.7, q: 15.21),
        EQBand(type: .peaking, freq: 405.0, gain: -6.4, q: 7.29),
        EQBand(type: .peaking, freq: 3772.0, gain: -3.7, q: 3.62),
        EQBand(type: .peaking, freq: 553.0, gain: -3.0, q: 3.1),
        EQBand(type: .peaking, freq: 49.1, gain: -7.2, q: 11.79),
        EQBand(type: .peaking, freq: 231.0, gain: -7.3, q: 13.12),
        EQBand(type: .peaking, freq: 272.0, gain: -4.0, q: 11.83),
      ])

    eqPresets = [headphone, roomLeft, roomRight]
    saveEQPresets()
  }
}
