import Foundation
import Testing

@testable import DSPAudio
@testable import DSPDoP

@Suite struct DoPTests {

  @Test func DoPDetectionAndBypass() throws {
    let multipliers = [64, 128, 256]
    let baseRates = [44100.0, 48000.0]

    for mult in multipliers {
      for baseRate in baseRates {
        let pcmSampleRate = baseRate * Double(mult) / 16.0
        let decoder = DoPDecoder(channels: 2, sampleRate: pcmSampleRate, bypassDoP: false)
        let chunk = AudioChunk(frames: 64, channels: 2)

        for t in 0..<64 {
          let marker: UInt32 = (t % 2 == 0) ? 0x05 : 0xFA
          let val24: UInt32 = (marker << 16) | 0x1234
          let intVal = Int32(bitPattern: val24 << 8) >> 8
          let floatVal = PrcFmt(intVal) / 8388608.0
          chunk[0][t] = floatVal
          chunk[1][t] = floatVal
        }

        var partChunk = AudioChunk(frames: 20, channels: 2)
        for ch in 0..<2 {
          for t in 0..<20 {
            partChunk[ch][t] = chunk[ch][t]
          }
        }
        var isDecoded = try decoder.detectAndProcess(chunk: &partChunk)
        #expect(!isDecoded)
        #expect(!decoder.isDoPActive)

        var partChunk2 = AudioChunk(frames: 44, channels: 2)
        for ch in 0..<2 {
          for t in 0..<44 {
            partChunk2[ch][t] = chunk[ch][t + 20]
          }
        }
        isDecoded = try decoder.detectAndProcess(chunk: &partChunk2)
        #expect(isDecoded)
        #expect(decoder.isDoPActive)

        let bypassedDecoder = DoPDecoder(
          channels: 2, sampleRate: pcmSampleRate, bypassDoP: true)
        var testChunk = chunk
        let processed = try bypassedDecoder.detectAndProcess(chunk: &testChunk)
        #expect(!processed)
        #expect(!bypassedDecoder.isDoPActive)
      }
    }
  }

  @Test func DoPFalsePositives() throws {
    let multipliers = [64, 128, 256]
    let baseRates = [44100.0, 48000.0]

    for mult in multipliers {
      for baseRate in baseRates {
        let pcmSampleRate = baseRate * Double(mult) / 16.0
        let decoder = DoPDecoder(channels: 1, sampleRate: pcmSampleRate, bypassDoP: false)
        var chunk = AudioChunk(frames: 64, channels: 1)

        for t in 0..<64 { chunk[0][t] = 0.0 }
        let res1 = try decoder.detectAndProcess(chunk: &chunk)
        #expect(!res1)

        for t in 0..<64 {
          chunk[0][t] = PrcFmt.random(in: -1.0...1.0)
        }
        let res2 = try decoder.detectAndProcess(chunk: &chunk)
        #expect(!res2)
      }
    }
  }

  @Test func MultiChunkDoPStreamStability() throws {
    let multipliers = [64, 128, 256]
    let baseRates = [44100.0, 48000.0]

    for mult in multipliers {
      for baseRate in baseRates {
        let pcmSampleRate = baseRate * Double(mult) / 16.0
        let decoder = DoPDecoder(channels: 2, sampleRate: pcmSampleRate, bypassDoP: false)
        let chunkSize = 1024
        let numChunks = 10

        var globalFrameIdx = 0
        for chunkIdx in 1...numChunks {
          var chunk = AudioChunk(frames: chunkSize, channels: 2)

          for t in 0..<chunkSize {
            let marker: UInt32 = (globalFrameIdx % 2 == 0) ? 0x05 : 0xFA
            let val24: UInt32 = (marker << 16) | 0x4321
            let intVal = Int32(bitPattern: val24 << 8) >> 8
            let floatVal = PrcFmt(intVal) / 8388608.0

            chunk[0][t] = floatVal
            chunk[1][t] = floatVal
            globalFrameIdx += 1
          }

          let processed = try decoder.detectAndProcess(chunk: &chunk)

          if chunkIdx == 1 {
            #expect(processed, "Chunk 1 failed to detect DoP stream for DSD\(mult) @ \(baseRate)!")
            #expect(decoder.isDoPActive, "Decoder failed to activate DoP active state!")
          } else {
            #expect(
              processed,
              "Stream broke or toggled at Chunk \(chunkIdx) for DSD\(mult) @ \(baseRate)!")
            #expect(
              decoder.isDoPActive, "Decoder dropped out of DoP active state at Chunk \(chunkIdx)!")
          }
        }
      }
    }
  }

  /// Full PCM → DoP → PCM roundtrip. Generates a 1 kHz tone at the PCM
  /// carrier rate, encodes through `DoPEncoder`, decodes through
  /// `DoPDecoder`, and measures SINAD on the recovered PCM. Exercises the
  /// encoder's polyphase interpolator + sigma-delta modulator together
  /// with the decoder's detection state machine and decimation filter.
  @Test func DoPRoundtripSINAD() throws {
    let multipliers = [64, 128, 256]
    let baseRates = [44100.0, 48000.0]

    for mult in multipliers {
      for baseRate in baseRates {
        let pcmSampleRate = baseRate * Double(mult) / 16.0
        let encoder = DoPEncoder(
          channels: 1, sampleRate: pcmSampleRate, outputDoP: true)
        let decoder = DoPDecoder(channels: 1, sampleRate: pcmSampleRate, bypassDoP: false)

        // Settle window covers (a) the decoder's 32-frame DoP lock-on,
        // (b) the SDM startup transient, and (c) the polyphase
        // interpolator/decimator group delays. 4 cycles of the test
        // tone is generous at all rates.
        let framesPerCycle = pcmSampleRate / 1000.0
        let activeFrames = Int(round(framesPerCycle * 10.0))
        let settleFrames = Int(round(framesPerCycle * 4.0))
        let frames = settleFrames + activeFrames

        // Generate a 1 kHz PCM sine at -3 dBFS at the carrier rate. -3 dB
        // (rather than full-scale) leaves the SDM headroom — at full
        // amplitude the noise shaper can saturate near peaks and degrade
        // SINAD by several dB.
        var chunk = AudioChunk(frames: frames, channels: 1)
        let amplitude: PrcFmt = 0.7071
        for t in 0..<frames {
          chunk[0][t] = amplitude * PrcFmt(sin(2.0 * .pi * 1000.0 * Double(t) / pcmSampleRate))
        }

        encoder.encode(chunk: &chunk)

        let processed = try decoder.detectAndProcess(chunk: &chunk)
        #expect(processed, "Failed DoP detection for DSD\(mult) @ \(baseRate)")
        #expect(decoder.isDoPActive, "DoP inactive for DSD\(mult) @ \(baseRate)")

        let targetFreq = 1000.0
        var cosSum = 0.0
        var sinSum = 0.0
        for t in settleFrames..<frames {
          let angle = 2.0 * .pi * targetFreq * Double(t) / pcmSampleRate
          cosSum += chunk[0][t] * cos(angle)
          sinSum += chunk[0][t] * sin(angle)
        }
        let cosAmp = (2.0 / Double(activeFrames)) * cosSum
        let sinAmp = (2.0 / Double(activeFrames)) * sinSum
        let fundamentalPower = (cosAmp * cosAmp + sinAmp * sinAmp) / 2.0

        var totalPower = 0.0
        for t in settleFrames..<frames {
          totalPower += chunk[0][t] * chunk[0][t]
        }
        totalPower /= Double(activeFrames)

        let noisePower = max(1e-20, totalPower - fundamentalPower)
        let sinad = 10.0 * log10(fundamentalPower / noisePower)

        // Thresholds are conservative: the roundtrip compounds encoder
        // SDM noise and decoder decimator passband ripple. Tighten if
        // measurements show consistent headroom.
        let expectedMinSinad: Double
        switch mult {
        case 64: expectedMinSinad = 90.0
        case 128: expectedMinSinad = 110.0
        case 256: expectedMinSinad = 115.0
        default: expectedMinSinad = 80.0
        }

        #expect(
          sinad >= expectedMinSinad,
          "Roundtrip SINAD too low (\(sinad) dB) for DSD\(mult) @ \(baseRate)")
      }
    }
  }
}
