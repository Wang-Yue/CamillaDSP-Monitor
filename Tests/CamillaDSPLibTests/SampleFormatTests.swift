// Sample format tokens accepted by the CoreAudio backend. Any name
// other than the four canonical CamillaDSP tokens (S16/S24/S32/F32) must
// fail to decode — there are intentionally no aliases.

import XCTest

@testable import CamillaDSPLib

final class SampleFormatTests: XCTestCase {

  func testCanonicalRawValues() {
    XCTAssertEqual(SampleFormat.s16.rawValue, "S16")
    XCTAssertEqual(SampleFormat.s24.rawValue, "S24")
    XCTAssertEqual(SampleFormat.s32.rawValue, "S32")
    XCTAssertEqual(SampleFormat.f32.rawValue, "F32")
  }

  func testDecodesCanonicalNames() throws {
    for name in ["S16", "S24", "S32", "F32"] {
      let json = "\"\(name)\""
      let decoded = try JSONDecoder().decode(
        SampleFormat.self, from: Data(json.utf8)
      )
      XCTAssertEqual(decoded.rawValue, name)
    }
  }

  func testRejectsAliases() {
    // Aliases that other camilladsp backends use ("S16LE", "F32_LE",
    // "FLOAT32LE", etc.) must not decode here — CoreAudio is strict.
    for alias in ["S16LE", "S24LE", "S32LE", "FLOAT32LE", "F32_LE", "S16_LE", "FLOAT64LE", "s16"] {
      let json = "\"\(alias)\""
      XCTAssertThrowsError(
        try JSONDecoder().decode(SampleFormat.self, from: Data(json.utf8)),
        "alias '\(alias)' should be rejected"
      )
    }
  }

  func testAllCases() {
    XCTAssertEqual(SampleFormat.allCases.count, 4)
  }
}
