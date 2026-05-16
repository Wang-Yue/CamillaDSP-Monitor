// REW / FRD / UMIK-format frequency-response curve loader.
//
// The de-facto interchange format for calibration files (mic
// response, headphone target curves, room measurements) is a
// plain-text two- or three-column file:
//
//   * comment lines start with '*' or '#' or ';'
//   * data lines: frequency_hz, magnitude_db [, phase_deg]
//   * columns separated by any whitespace (REW writes with spaces,
//     miniDSP UMIK writes tabs, hand-edited files vary)
//   * frequencies in ascending order
//
// Common variants this loader handles transparently:
//
//   - **REW exports** — `* commented` header, then space-separated
//     data rows.
//   - **miniDSP UMIK-1 / UMIK-2** — first line is a quoted header
//     `"Sens Factor =-1.6dB, SERNO: 9000123"` (no leading `*`),
//     then tab-separated `freq\tdB` rows. The Sens Factor is
//     skipped; mic-sensitivity calibration is a level offset, not a
//     frequency-response correction, and the auto-fitter normalises
//     levels independently anyway.
//   - **AutoEq** target curves — same shape, sometimes with
//     additional columns we ignore.
//
// Anything that isn't a comment line and doesn't start with a
// numeric first field is treated as an unrecognised header and
// silently skipped. Lines whose first field IS numeric but whose
// second field isn't are reported as malformed (so genuine errors
// don't pass silently).
//
// The parsed curve carries log-spaced data and exposes
// `magnitude(at:)` / `phase(at:)` evaluators that interpolate in
// log-frequency space (matching how the rest of the measurement
// pipeline operates).

import DSPAudio
import Foundation

public struct CalibrationCurve: Sendable {
  /// Source frequencies in ascending order, Hz.
  public let frequencies: [PrcFmt]
  /// Magnitudes at each frequency, dB.
  public let magnitudesDB: [PrcFmt]
  /// Phases at each frequency, degrees. `nil` when the source file
  /// only had two columns (magnitude only).
  public let phasesDeg: [PrcFmt]?

  public init(
    frequencies: [PrcFmt],
    magnitudesDB: [PrcFmt],
    phasesDeg: [PrcFmt]? = nil
  ) {
    precondition(frequencies.count == magnitudesDB.count)
    if let p = phasesDeg {
      precondition(p.count == frequencies.count)
    }
    self.frequencies = frequencies
    self.magnitudesDB = magnitudesDB
    self.phasesDeg = phasesDeg
  }

  /// Magnitude at `f` Hz, log-frequency-linearly interpolated.
  /// Constant-extrapolates outside the measured range.
  public func magnitude(at f: PrcFmt) -> PrcFmt {
    Self.interpolate(at: f, frequencies: frequencies, values: magnitudesDB)
  }

  /// Phase at `f` Hz in degrees, log-frequency-linearly interpolated.
  /// Returns 0 when the curve has no phase data.
  public func phase(at f: PrcFmt) -> PrcFmt {
    guard let p = phasesDeg else { return 0 }
    return Self.interpolate(at: f, frequencies: frequencies, values: p)
  }

  /// Sample the curve onto an arbitrary frequency grid (typically
  /// the analysis grid). Returns dB magnitudes parallel to `grid`.
  public func sampledMagnitudeDB(at grid: [PrcFmt]) -> [PrcFmt] {
    grid.map { magnitude(at: $0) }
  }

  // MARK: - Loading

  public enum LoadError: Error, CustomStringConvertible {
    case fileNotFound(String)
    case readFailed(String)
    case noDataLines(String)
    case malformedLine(line: Int, content: String)

    public var description: String {
      switch self {
      case .fileNotFound(let p): return "Calibration file not found: \(p)"
      case .readFailed(let m): return "Calibration read failed: \(m)"
      case .noDataLines(let p): return "No data lines in calibration file: \(p)"
      case .malformedLine(let line, let content):
        return "Malformed line \(line): \(content)"
      }
    }
  }

  /// Parse a `.frd` / `.txt` file. Throws `LoadError` on parse
  /// failures; ignores malformed *comment* lines but rejects
  /// malformed *data* lines so the caller knows the file isn't
  /// trustworthy.
  public static func load(at path: String) throws -> CalibrationCurve {
    let url = URL(fileURLWithPath: path)
    guard FileManager.default.fileExists(atPath: path) else {
      throw LoadError.fileNotFound(path)
    }
    let text: String
    do {
      text = try String(contentsOf: url, encoding: .utf8)
    } catch {
      throw LoadError.readFailed(String(describing: error))
    }
    return try parse(text, sourcePath: path)
  }

  /// Parse calibration data already loaded into a `String`. Useful
  /// for embedded resources / tests / paste-from-clipboard.
  public static func parse(_ text: String, sourcePath: String = "<inline>")
    throws -> CalibrationCurve
  {
    var freqs: [PrcFmt] = []
    var mags: [PrcFmt] = []
    var phases: [PrcFmt] = []
    var sawPhaseColumn = false

    let cleanText = text.replacingOccurrences(of: "\r", with: "")
    let lines = cleanText.split(separator: "\n", omittingEmptySubsequences: false)
    for (idx, raw) in lines.enumerated() {
      let trimmed = raw.trimmingCharacters(in: .whitespaces)
      if trimmed.isEmpty { continue }
      // Comment lines.
      if trimmed.hasPrefix("*") || trimmed.hasPrefix("#") || trimmed.hasPrefix(";") {
        continue
      }
      // Data lines — split on any whitespace.
      let fields = trimmed.split(whereSeparator: { $0.isWhitespace })
      guard fields.count >= 2,
        let f = PrcFmt(fields[0]),
        let m = PrcFmt(fields[1])
      else {
        // Some FRD files have a "Sensitivity" or other text header
        // line before the data starts; skip if the first field isn't
        // numeric. Only reject if the first line that LOOKS numeric
        // is malformed.
        if PrcFmt(fields.first.map(String.init) ?? "") != nil {
          throw LoadError.malformedLine(line: idx + 1, content: String(trimmed))
        }
        continue
      }
      freqs.append(f)
      mags.append(m)
      if fields.count >= 3, let p = PrcFmt(fields[2]) {
        phases.append(p)
        sawPhaseColumn = true
      } else if sawPhaseColumn {
        // Some lines had phase, this one doesn't — treat as 0.
        phases.append(0)
      }
    }
    if freqs.isEmpty {
      throw LoadError.noDataLines(sourcePath)
    }
    // Ensure ascending freq order; if a file has them descending we
    // sort rather than reject (some hardware tools dump that way).
    if zip(freqs, freqs.dropFirst()).contains(where: { $0 > $1 }) {
      let sorted = zip(freqs, mags).sorted { $0.0 < $1.0 }
      freqs = sorted.map(\.0)
      mags = sorted.map(\.1)
      // Phases would need to be re-sorted in lock-step; if we hit
      // this branch with phase data, just drop it rather than risk
      // mis-aligning.
      sawPhaseColumn = false
      phases = []
    }
    return CalibrationCurve(
      frequencies: freqs,
      magnitudesDB: mags,
      phasesDeg: sawPhaseColumn ? phases : nil)
  }

  // MARK: - Export

  /// Write the curve to disk in the standard FRD format. Header
  /// lines begin with `*` so REW and similar tools skip them. If the
  /// curve has no phase data, only two columns are written.
  public func writeFRD(to path: String, comment: String? = nil) throws {
    var lines: [String] = []
    lines.append("* Frequency Response Data")
    lines.append("* Exported by DSPMonitor")
    if let c = comment, !c.isEmpty {
      for line in c.split(separator: "\n") {
        lines.append("* \(line)")
      }
    }
    if phasesDeg != nil {
      lines.append("* Frequency [Hz]   Magnitude [dB]   Phase [deg]")
    } else {
      lines.append("* Frequency [Hz]   Magnitude [dB]")
    }
    for i in 0..<frequencies.count {
      if let p = phasesDeg?[i] {
        lines.append(
          String(format: "%.6f %.6f %.6f", frequencies[i], magnitudesDB[i], p))
      } else {
        lines.append(
          String(format: "%.6f %.6f", frequencies[i], magnitudesDB[i]))
      }
    }
    let text = lines.joined(separator: "\n") + "\n"
    try text.write(toFile: path, atomically: true, encoding: .utf8)
  }

  // MARK: - Helpers

  /// Log-frequency-linear interpolation. Values are interpolated as
  /// if both axes were `(log10(f), value)`. Constant-extrapolates.
  private static func interpolate(
    at f: PrcFmt, frequencies: [PrcFmt], values: [PrcFmt]
  ) -> PrcFmt {
    let n = frequencies.count
    guard n > 0 else { return 0 }
    if n == 1 { return values[0] }
    if f <= frequencies[0] { return values[0] }
    if f >= frequencies[n - 1] { return values[n - 1] }
    // Binary search for the bracketing pair.
    var lo = 0
    var hi = n - 1
    while hi - lo > 1 {
      let mid = (lo + hi) / 2
      if frequencies[mid] <= f { lo = mid } else { hi = mid }
    }
    let fLo = frequencies[lo]
    let fHi = frequencies[hi]
    let vLo = values[lo]
    let vHi = values[hi]
    let logF = log10(f)
    let logLo = log10(fLo)
    let logHi = log10(fHi)
    let t = (logF - logLo) / (logHi - logLo)
    return vLo + t * (vHi - vLo)
  }
}
