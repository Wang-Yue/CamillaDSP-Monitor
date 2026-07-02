import Foundation
import Testing

@testable import DSPAudio
@testable import DSPConfig
@testable import DSPFilters

@Suite struct BiquadComboTests {
  private static func isClose(_ left: PrcFmt, _ right: PrcFmt, maxdiff: PrcFmt) -> Bool {
    return abs(left - right) < maxdiff
  }

  private static func compareVecs(_ left: [PrcFmt], _ right: [PrcFmt], maxdiff: PrcFmt) -> Bool {
    guard left.count == right.count else { return false }
    for (val_l, val_r) in zip(left, right) {
      if !isClose(val_l, val_r, maxdiff: maxdiff) {
        return false
      }
    }
    return true
  }

  @Test func make_butterworth_2() {
    let q = BiquadComboFilter.butterworthQ(order: 2)
    let expect: [PrcFmt] = [0.707]
    #expect(q.count == 1)
    #expect(Self.compareVecs(q, expect, maxdiff: 0.01))
  }

  @Test func make_butterworth_5() {
    let q = BiquadComboFilter.butterworthQ(order: 5)
    let expect: [PrcFmt] = [1.62, 0.62, -1.0]
    #expect(q.count == 3)
    #expect(Self.compareVecs(q, expect, maxdiff: 0.01))
  }

  @Test func make_butterworth_8() {
    let q = BiquadComboFilter.butterworthQ(order: 8)
    let expect: [PrcFmt] = [2.56, 0.9, 0.6, 0.51]
    #expect(q.count == 4)
    #expect(Self.compareVecs(q, expect, maxdiff: 0.01))
  }

  @Test func make_lr4() {
    let q = BiquadComboFilter.linkwitzRileyQ(order: 4)
    let expect: [PrcFmt] = [0.707, 0.707]
    #expect(q.count == 2)
    #expect(Self.compareVecs(q, expect, maxdiff: 0.01))
  }

  @Test func make_lr6() {
    // Note: Rust test checks 10 order
    let q = BiquadComboFilter.linkwitzRileyQ(order: 10)
    let expect: [PrcFmt] = [1.62, 0.62, 1.62, 0.62, 0.5]
    #expect(q.count == 5)
    #expect(Self.compareVecs(q, expect, maxdiff: 0.01))
  }

  @Test func check_lr() {
    let fs = 48000
    let okconf = BiquadComboParameters(
      type: .linkwitzRileyHighpass,
      freq: 1000.0,
      order: 6
    )
    #expect(throws: Never.self) { try okconf.validate(sampleRate: fs) }

    let badconf1 = BiquadComboParameters(
      type: .linkwitzRileyHighpass,
      freq: 1000.0,
      order: 5
    )
    #expect(throws: Error.self) { try badconf1.validate(sampleRate: fs) }

    let badconf2 = BiquadComboParameters(
      type: .linkwitzRileyHighpass,
      freq: 1000.0,
      order: 0
    )
    #expect(throws: Error.self) { try badconf2.validate(sampleRate: fs) }

    let badconf3 = BiquadComboParameters(
      type: .linkwitzRileyHighpass,
      freq: 0.0,
      order: 2
    )
    #expect(throws: Error.self) { try badconf3.validate(sampleRate: fs) }

    let badconf4 = BiquadComboParameters(
      type: .linkwitzRileyHighpass,
      freq: 25000.0,
      order: 2
    )
    #expect(throws: Error.self) { try badconf4.validate(sampleRate: fs) }
  }

  @Test func check_butterworth() {
    let fs = 48000
    let okconf1 = BiquadComboParameters(
      type: .butterworthHighpass,
      freq: 1000.0,
      order: 6
    )
    #expect(throws: Never.self) { try okconf1.validate(sampleRate: fs) }

    let okconf2 = BiquadComboParameters(
      type: .butterworthHighpass,
      freq: 1000.0,
      order: 5
    )
    #expect(throws: Never.self) { try okconf2.validate(sampleRate: fs) }

    let badconf = BiquadComboParameters(
      type: .butterworthHighpass,
      freq: 1000.0,
      order: 0
    )
    #expect(throws: Error.self) { try badconf.validate(sampleRate: fs) }

    let badconf3 = BiquadComboParameters(
      type: .butterworthHighpass,
      freq: 0.0,
      order: 2
    )
    #expect(throws: Error.self) { try badconf3.validate(sampleRate: fs) }

    let badconf4 = BiquadComboParameters(
      type: .butterworthHighpass,
      freq: 25000.0,
      order: 2
    )
    #expect(throws: Error.self) { try badconf4.validate(sampleRate: fs) }
  }
}
