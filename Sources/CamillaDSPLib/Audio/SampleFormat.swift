// Sample format tokens accepted by CamillaDSP's CoreAudio backend.
//
// On macOS, CoreAudio is the only I/O path CamillaDSP-Monitor uses, and the
// upstream `CoreAudioSampleFormat` enum (config/mod.rs in the camilladsp
// `next4.2.0` branch) defines exactly four variants — `S16`, `S24`, `S32`,
// `F32`. We mirror that set verbatim with no aliases, so a misspelt format
// fails at parse time rather than silently mapping to something else.

import Foundation

public enum SampleFormat: String, Codable, CaseIterable, Sendable {
  case s16 = "S16"
  case s24 = "S24"
  case s32 = "S32"
  case f32 = "F32"
}
