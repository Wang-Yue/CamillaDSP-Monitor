// Sample format tokens accepted by the CoreAudio backend. Any name
// other than the four canonical CamillaDSP tokens (S16/S24/S32/F32) must
// fail to decode — there are intentionally no aliases.

import Foundation
import Testing

@testable import DSPConfig

@Suite struct SampleFormatTests {

  @Test func CanonicalRawValues() {
    #expect(SampleFormat.s16.rawValue == "S16")
    #expect(SampleFormat.s24.rawValue == "S24")
    #expect(SampleFormat.s32.rawValue == "S32")
    #expect(SampleFormat.f32.rawValue == "F32")
  }

  @Test func DecodesCanonicalNames() throws {
    for name in ["S16", "S24", "S32", "F32"] {
      let json = "\"\(name)\""
      let decoded = try JSONDecoder().decode(
        SampleFormat.self, from: Data(json.utf8)
      )
      #expect(decoded.rawValue == name)
    }
  }

  @Test func RejectsAliases() {
    // Aliases that other camilladsp backends use ("S16LE", "F32_LE",
    // "FLOAT32LE", etc.) must not decode here — CoreAudio is strict.
    for alias in ["S16LE", "S24LE", "S32LE", "FLOAT32LE", "F32_LE", "S16_LE", "FLOAT64LE", "s16"] {
      let json = "\"\(alias)\""
      do {
        _ = try JSONDecoder().decode(SampleFormat.self, from: Data(json.utf8))
        Issue.record("Expected error to be thrown")
      } catch {
        // expected exception
      }
    }
  }

  @Test func AllCases() {
    #expect(SampleFormat.allCases.count == 4)
  }
}
