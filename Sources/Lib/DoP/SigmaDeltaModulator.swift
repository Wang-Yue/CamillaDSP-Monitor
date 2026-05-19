// Sigma-delta modulator (DSD encoder core).
// The per-sample `processSample` entry point lets
// the encoder feed one oversampled value at a time cleanly and efficiently.

import DSPAudio
import DSPConfig
import Foundation

public let SAMPLE_MAX: Double = 2147483647.0

public final class SigmaDeltaModulator: @unchecked Sendable {
  public struct SDMPreset: Sendable {
    public let a: [Double]
    public let g: [Double]
    public let order: Int32
    public let freq: UInt32
    public let name: SDMFilter
  }

  @usableFromInline var idx: Int = 0
  @usableFromInline var prevY: Double = 0.0

  /// Heap-backed fixed storage for the non-trellis path's two-state
  /// ping-pong. Layout: `nonTrellisState[0..7]` is slot 0,
  /// `nonTrellisState[8..15]` is slot 1; `idx ∈ {0,1}` selects which slot
  /// is current. Replaces the `SDMState.state` `[Double]` arrays the
  /// per-sample loop used to mutate via `inout`, which triggered an Array
  /// CoW allocation on every sample.
  ///
  /// `cachedA` / `cachedG` mirror the selected filter's `a` and `g`
  /// coefficients into pointer storage so the hot loop avoids re-copying
  /// the SDMFilter struct (and its `[Double]` ARC traffic) on every call.
  @usableFromInline let nonTrellisState: UnsafeMutablePointer<Double>
  @usableFromInline let cachedA: UnsafeMutablePointer<Double>
  @usableFromInline let cachedG: UnsafeMutablePointer<Double>
  @usableFromInline let cachedOrder: Int

  public static let sdmFilters: [SDMPreset] = [
    // MARK: - 256x Rate Filters
    SDMPreset(
      a: [
        1.00323940832478e+00, 3.54975562370606e-01, 5.64754047673194e-02, 3.99067228430322e-03, 0,
        0, 0, 0,
      ],
      g: [1.74071110561285e-05, 0, 1.11672812199443e-04, 0, 0, 0, 0, 0], order: 4,
      freq: 256 * 44100, name: .clans4),
    SDMPreset(
      a: [
        8.69746397840960e-01, 3.58080546314756e-01, 8.02654082306273e-02, 8.06528716282692e-03, 0,
        0, 0, 0,
      ],
      g: [1.74071110561285e-05, 0, 1.11672812199443e-04, 0, 0, 0, 0, 0], order: 4,
      freq: 256 * 44100, name: .sdm4),
    SDMPreset(
      a: [
        1.10212073518628e+00, 4.33447134954244e-01, 7.17865111532609e-02, 4.48367825425951e-03,
        8.60861641068938e-05, 0, 0, 0,
      ],
      g: [0, 4.36651951230006e-05, 0, 1.23660417994961e-04, 0, 0, 0, 0], order: 5,
      freq: 256 * 44100, name: .clans5),
    SDMPreset(
      a: [
        8.07768375734983e-01, 3.16440095967511e-01, 7.38231738259889e-02, 1.01432044963374e-02,
        6.46658652275506e-04, 0, 0, 0,
      ],
      g: [0, 4.36651951230006e-05, 0, 1.23660417994961e-04, 0, 0, 0, 0], order: 5,
      freq: 256 * 44100, name: .sdm5),
    SDMPreset(
      a: [
        9.97000121097967e-01, 3.46002867430604e-01, 5.74352078895161e-02, 4.96197900435677e-03,
        2.16319301330580e-04, 3.45938007947910e-06, 0, 0,
      ],
      g: [8.57500543083848e-06, 0, 6.58398680532347e-05, 0, 1.30939362595793e-04, 0, 0, 0],
      order: 6,
      freq: 256 * 44100, name: .clans6),
    SDMPreset(
      a: [
        8.08851952379691e-01, 3.20414766828429e-01, 7.85858596284593e-02, 1.24781319607895e-02,
        1.21202847406105e-03, 5.51622876557856e-05, 0, 0,
      ],
      g: [8.57500543083848e-06, 0, 6.58398680532347e-05, 0, 1.30939362595793e-04, 0, 0, 0],
      order: 6,
      freq: 256 * 44100, name: .sdm6),
    SDMPreset(
      a: [
        1.10629931445134e+00, 4.22135693734657e-01, 7.54595882135669e-02, 7.07164815703843e-03,
        3.53092575577382e-04, 8.89662856104825e-06, 5.79674109824069e-08, 0,
      ],
      g: [0, 2.48046933669715e-05, 0, 8.28068362972358e-05, 0, 1.35653594733585e-04, 0, 0],
      order: 7,
      freq: 256 * 44100, name: .clans7),
    SDMPreset(
      a: [
        7.82785077952658e-01, 3.01888671316811e-01, 7.36594376027782e-02, 1.22068270909817e-02,
        1.36572694403914e-03, 9.57806082134936e-05, 3.13239368043838e-06, 0,
      ],
      g: [0, 2.48046933669715e-05, 0, 8.28068362972358e-05, 0, 1.35653594733585e-04, 0, 0],
      order: 7,
      freq: 256 * 44100, name: .sdm7),
    SDMPreset(
      a: [
        1.15188624720851e+00, 5.45054196257555e-01, 1.38703640845632e-01, 2.07076444822072e-02,
        1.85506614417771e-03, 9.63403135615390e-05, 2.69174565706992e-06, 2.22594461751768e-08,
      ],
      g: [
        5.06749566262594e-06, 0, 4.15924517416912e-05, 0, 9.55783346944871e-05, 0,
        1.38868728742641e-04, 0,
      ], order: 8,
      freq: 256 * 44100, name: .clans8),
    SDMPreset(
      a: [
        7.42329617949054e-01, 2.72509195471757e-01, 6.41424039739473e-02, 1.05299412132258e-02,
        1.23178223428228e-03, 9.94985029720342e-05, 5.13169547054423e-06, 1.20466411041020e-07,
      ],
      g: [
        5.06749566262594e-06, 0, 4.15924517416912e-05, 0, 9.55783346944871e-05, 0,
        1.38868728742641e-04, 0,
      ], order: 8,
      freq: 256 * 44100, name: .sdm8),

    // MARK: - 128x Rate Filters
    SDMPreset(
      a: [
        1.19985242167687e+00, 5.39366678861047e-01, 1.07433710905069e-01, 7.85649993434925e-03, 0,
        0, 0, 0,
      ],
      g: [6.96272321944526e-05, 0, 4.46641365529834e-04, 0, 0, 0, 0, 0], order: 4,
      freq: 128 * 44100, name: .clans4),
    SDMPreset(
      a: [
        8.69935494013007e-01, 3.57844753369190e-01, 8.00232187246903e-02, 7.95176796646842e-03, 0,
        0, 0, 0,
      ],
      g: [6.96272321944526e-05, 0, 4.46641365529834e-04, 0, 0, 0, 0, 0], order: 4,
      freq: 128 * 44100, name: .sdm4),
    SDMPreset(
      a: [
        1.12849522129362e+00, 5.02128177800632e-01, 1.10084368682902e-01, 1.18635667860902e-02,
        4.71059243536326e-04, 0, 0, 0,
      ],
      g: [0, 1.74653153894942e-04, 0, 4.94580504383930e-04, 0, 0, 0, 0], order: 5,
      freq: 128 * 44100, name: .clans5),
    SDMPreset(
      a: [
        8.08016362125685e-01, 3.16129639744972e-01, 7.34835047943110e-02, 1.00377576971692e-02,
        6.20309683440734e-04, 0, 0, 0,
      ],
      g: [0, 1.74653153894942e-04, 0, 4.94580504383930e-04, 0, 0, 0, 0], order: 5,
      freq: 128 * 44100, name: .sdm5),
    SDMPreset(
      a: [
        1.13839804508630e+00, 5.16338264778321e-01, 1.20760874713903e-01, 1.53496744585395e-02,
        1.00733946588732e-03, 2.18223963130981e-05, 0, 0,
      ],
      g: [3.42997276004814e-05, 0, 2.63342132660038e-04, 0, 5.23688869916463e-04, 0, 0, 0],
      order: 6,
      freq: 128 * 44100, name: .clans6),
    SDMPreset(
      a: [
        8.09157514480151e-01, 3.20038611545599e-01, 7.81955723892726e-02, 1.23074728674017e-02,
        1.18346416730106e-03, 5.04301224894810e-05, 0, 0,
      ],
      g: [3.42997276004814e-05, 0, 2.63342132660038e-04, 0, 5.23688869916463e-04, 0, 0, 0],
      order: 6,
      freq: 128 * 44100, name: .sdm6),
    SDMPreset(
      a: [
        8.98180853333862e-01, 3.27985497323439e-01, 6.38803466871112e-02, 7.18262647412857e-03,
        4.51845004995476e-04, 1.49685651672331e-05, 4.22554681245302e-08, 0,
      ],
      g: [0, 9.92163123766340e-05, 0, 3.31199917300393e-04, 0, 5.42540771343282e-04, 0, 0],
      order: 7,
      freq: 128 * 44100, name: .clans7),
    SDMPreset(
      a: [
        7.83148010097334e-01, 3.01437231238902e-01, 7.31891646224574e-02, 1.20314098366875e-02,
        1.32077937193861e-03, 9.11181979169687e-05, 2.59895240306562e-06, 0,
      ],
      g: [0, 9.92163123766340e-05, 0, 3.31199917300393e-04, 0, 5.42540771343282e-04, 0, 0],
      order: 7,
      freq: 128 * 44100, name: .sdm7),
    SDMPreset(
      a: [
        1.04472698053970e+00, 4.62088167600438e-01, 1.13484722685479e-01, 1.68939738398161e-02,
        1.55891676875336e-03, 8.23864822188133e-05, 2.39690238375972e-06, -1.75063180618551e-09,
      ],
      g: [
        2.02698799324546e-05, 0, 1.66362887238597e-04, 0, 3.82276797905696e-04, 0,
        5.55397776875272e-04, 0,
      ], order: 8,
      freq: 128 * 44100, name: .clans8),
    SDMPreset(
      a: [
        7.42763211426562e-01, 2.71983157679393e-01, 6.36389361390464e-02, 1.03289230528372e-02,
        1.19045645863092e-03, 9.25357160397986e-05, 4.64982367004083e-06, 8.14280266547840e-08,
      ],
      g: [
        2.02698799324546e-05, 0, 1.66362887238597e-04, 0, 3.82276797905696e-04, 0,
        5.55397776875272e-04, 0,
      ], order: 8,
      freq: 128 * 44100, name: .sdm8),

    // MARK: - 64x Rate Filters
    SDMPreset(
      a: [
        1.27879853057675e+00, 6.11303913722028e-01, 1.28497083869344e-01, 9.36669621421730e-03, 0,
        0, 0, 0,
      ],
      g: [2.78489536971958e-04, 0, 1.78576750808173e-03, 0, 0, 0, 0, 0], order: 4, freq: 64 * 44100,
      name: .clans4),
    SDMPreset(
      a: [
        8.70691905361989e-01, 3.56902669565715e-01, 7.90540396115068e-02, 7.49922172520510e-03, 0,
        0, 0, 0,
      ],
      g: [2.78489536971958e-04, 0, 1.78576750808173e-03, 0, 0, 0, 0, 0], order: 4, freq: 64 * 44100,
      name: .sdm4),
    SDMPreset(
      a: [
        1.09979653514762e+00, 4.81149952106030e-01, 1.03481231987752e-01, 1.07520561970131e-02,
        3.08801118488355e-04, 0, 0, 0,
      ],
      g: [0, 6.98490600683106e-04, 0, 1.97734357803445e-03, 0, 0, 0, 0], order: 5, freq: 64 * 44100,
      name: .clans5),
    SDMPreset(
      a: [
        8.09008352716413e-01, 3.14889441429587e-01, 7.21235639855639e-02, 9.61769014140330e-03,
        5.16726747047100e-04, 0, 0, 0,
      ],
      g: [0, 6.98490600683106e-04, 0, 1.97734357803445e-03, 0, 0, 0, 0], order: 5, freq: 64 * 44100,
      name: .sdm5),
    SDMPreset(
      a: [
        1.07903996429881e+00, 4.81889508657128e-01, 1.12960470418260e-01, 1.41786764681378e-02,
        8.90696638761455e-04, 3.12209321540191e-06, 0, 0,
      ],
      g: [1.37194204516672e-04, 0, 1.05309113432481e-03, 0, 2.09365847953595e-03, 0, 0, 0],
      order: 6, freq: 64 * 44100,
      name: .clans6),
    SDMPreset(
      a: [
        8.10379824203071e-01, 3.18536209193388e-01, 7.66325098232035e-02, 1.16280270347611e-02,
        1.07113013239551e-03, 3.23564051386283e-05, 0, 0,
      ],
      g: [1.37194204516672e-04, 0, 1.05309113432481e-03, 0, 2.09365847953595e-03, 0, 0, 0],
      order: 6, freq: 64 * 44100,
      name: .sdm6),
    SDMPreset(
      a: [
        1.30828743581024e+00, 6.14252690035661e-01, 1.30284958810903e-01, 1.31280998331490e-02,
        4.80497172614556e-04, 1.28747977598542e-07, -1.01500259908072e-06, 0,
      ],
      g: [0, 3.96825873999969e-04, 0, 1.32436089566069e-03, 0, 2.16898568341885e-03, 0, 0],
      order: 7, freq: 64 * 44100,
      name: .clans7),
    SDMPreset(
      a: [
        7.84599817960974e-01, 2.99634346028983e-01, 7.13049276218066e-02, 1.13334107086916e-02,
        1.14497642818158e-03, 7.33018502803093e-05, 6.80633400002018e-07, 0,
      ],
      g: [0, 3.96825873999969e-04, 0, 1.32436089566069e-03, 0, 2.16898568341885e-03, 0, 0],
      order: 7, freq: 64 * 44100,
      name: .sdm7),
    SDMPreset(
      a: [
        1.18730059129261e+00, 5.66733317291325e-01, 1.40117339676942e-01, 1.87599862200771e-02,
        1.27685506908071e-03, 8.76397405988154e-06, -1.90294986721073e-06, -7.39020160622772e-08,
      ],
      g: [
        8.10778762576884e-05, 0, 6.65340842513387e-04, 0, 1.52852264942192e-03, 0,
        2.22035724073886e-03, 0,
      ], order: 8, freq: 64 * 44100,
      name: .clans8),
    SDMPreset(
      a: [
        7.44453769826547e-01, 2.69850507860307e-01, 6.16093616071757e-02, 9.52771711245796e-03,
        1.02903114196526e-03, 6.63758229311911e-05, 2.91124056073927e-06, -4.29323230577427e-08,
      ],
      g: [
        8.10778762576884e-05, 0, 6.65340842513387e-04, 0, 1.52852264942192e-03, 0,
        2.22035724073886e-03, 0,
      ], order: 8, freq: 64 * 44100,
      name: .sdm8),
  ]

  public static func sdmFindFilter(name: SDMFilter?, freq: UInt32) -> SDMPreset? {
    return SigmaDeltaModulator.sdmFilters.first { f in
      (name == nil || f.name == name) && f.freq <= freq
    }
  }

  public init?(
    filterName: SDMFilter?, freq: UInt32
  ) {
    guard let selectedFilter = SigmaDeltaModulator.sdmFindFilter(name: filterName, freq: freq)
    else {
      return nil
    }

    // Commit: from this point we don't fail, so it's safe to allocate
    // and own the heap buffers via the `let` properties + deinit.
    let nts = UnsafeMutablePointer<Double>.allocate(capacity: 16)
    nts.initialize(repeating: 0.0, count: 16)
    self.nonTrellisState = nts

    let aPtr = UnsafeMutablePointer<Double>.allocate(capacity: 8)
    let gPtr = UnsafeMutablePointer<Double>.allocate(capacity: 8)
    for i in 0..<8 {
      (aPtr + i).initialize(to: selectedFilter.a[i])
      (gPtr + i).initialize(to: selectedFilter.g[i])
    }
    self.cachedA = aPtr
    self.cachedG = gPtr
    self.cachedOrder = Int(selectedFilter.order)
  }

  deinit {
    nonTrellisState.deinitialize(count: 16)
    nonTrellisState.deallocate()
    cachedA.deinitialize(count: 8)
    cachedA.deallocate()
    cachedG.deinitialize(count: 8)
    cachedG.deallocate()
  }

  // MARK: - Public Specialized Math Helpers

  @inlinable
  @inline(__always)
  public func sdmSample4(_ x: Double) -> Double {
    let currentIdx = idx
    let s = nonTrellisState.advanced(by: currentIdx * 8)
    let d = nonTrellisState.advanced(by: (currentIdx ^ 1) * 8)
    let a = cachedA
    let g = cachedG
    let y = prevY

    d[0] = s[0] - g[0] * s[1] + x - y
    var v = x + a[0] * d[0]

    d[1] = s[1] + s[0] - g[1] * s[2]
    v += a[1] * d[1]

    d[2] = s[2] + s[1] - g[2] * s[3]
    v += a[2] * d[2]

    d[3] = s[3] + s[2]
    v += a[3] * d[3]

    let yNew = (v.sign == .minus) ? -1.0 : 1.0
    idx = currentIdx ^ 1
    prevY = yNew
    return yNew
  }

  @inlinable
  @inline(__always)
  public func sdmSample5(_ x: Double) -> Double {
    let currentIdx = idx
    let s = nonTrellisState.advanced(by: currentIdx * 8)
    let d = nonTrellisState.advanced(by: (currentIdx ^ 1) * 8)
    let a = cachedA
    let g = cachedG
    let y = prevY

    d[0] = s[0] - g[0] * s[1] + x - y
    var v = x + a[0] * d[0]

    d[1] = s[1] + s[0] - g[1] * s[2]
    v += a[1] * d[1]

    d[2] = s[2] + s[1] - g[2] * s[3]
    v += a[2] * d[2]

    d[3] = s[3] + s[2] - g[3] * s[4]
    v += a[3] * d[3]

    d[4] = s[4] + s[3]
    v += a[4] * d[4]

    let yNew = (v.sign == .minus) ? -1.0 : 1.0
    idx = currentIdx ^ 1
    prevY = yNew
    return yNew
  }

  @inlinable
  @inline(__always)
  public func sdmSample6(_ x: Double) -> Double {
    let currentIdx = idx
    let s = nonTrellisState.advanced(by: currentIdx * 8)
    let d = nonTrellisState.advanced(by: (currentIdx ^ 1) * 8)
    let a = cachedA
    let g = cachedG
    let y = prevY

    d[0] = s[0] - g[0] * s[1] + x - y
    var v = x + a[0] * d[0]

    d[1] = s[1] + s[0] - g[1] * s[2]
    v += a[1] * d[1]

    d[2] = s[2] + s[1] - g[2] * s[3]
    v += a[2] * d[2]

    d[3] = s[3] + s[2] - g[3] * s[4]
    v += a[3] * d[3]

    d[4] = s[4] + s[3] - g[4] * s[5]
    v += a[4] * d[4]

    d[5] = s[5] + s[4]
    v += a[5] * d[5]

    let yNew = (v.sign == .minus) ? -1.0 : 1.0
    idx = currentIdx ^ 1
    prevY = yNew
    return yNew
  }

  @inlinable
  @inline(__always)
  public func sdmSample7(_ x: Double) -> Double {
    let currentIdx = idx
    let s = nonTrellisState.advanced(by: currentIdx * 8)
    let d = nonTrellisState.advanced(by: (currentIdx ^ 1) * 8)
    let a = cachedA
    let g = cachedG
    let y = prevY

    d[0] = s[0] - g[0] * s[1] + x - y
    var v = x + a[0] * d[0]

    d[1] = s[1] + s[0] - g[1] * s[2]
    v += a[1] * d[1]

    d[2] = s[2] + s[1] - g[2] * s[3]
    v += a[2] * d[2]

    d[3] = s[3] + s[2] - g[3] * s[4]
    v += a[3] * d[3]

    d[4] = s[4] + s[3] - g[4] * s[5]
    v += a[4] * d[4]

    d[5] = s[5] + s[4] - g[5] * s[6]
    v += a[5] * d[5]

    d[6] = s[6] + s[5]
    v += a[6] * d[6]

    let yNew = (v.sign == .minus) ? -1.0 : 1.0
    idx = currentIdx ^ 1
    prevY = yNew
    return yNew
  }

  @inlinable
  @inline(__always)
  public func sdmSample8(_ x: Double) -> Double {
    let currentIdx = idx
    let s = nonTrellisState.advanced(by: currentIdx * 8)
    let d = nonTrellisState.advanced(by: (currentIdx ^ 1) * 8)
    let a = cachedA
    let g = cachedG
    let y = prevY

    d[0] = s[0] - g[0] * s[1] + x - y
    var v = x + a[0] * d[0]

    d[1] = s[1] + s[0] - g[1] * s[2]
    v += a[1] * d[1]

    d[2] = s[2] + s[1] - g[2] * s[3]
    v += a[2] * d[2]

    d[3] = s[3] + s[2] - g[3] * s[4]
    v += a[3] * d[3]

    d[4] = s[4] + s[3] - g[4] * s[5]
    v += a[4] * d[4]

    d[5] = s[5] + s[4] - g[5] * s[6]
    v += a[5] * d[5]

    d[6] = s[6] + s[5] - g[6] * s[7]
    v += a[6] * d[6]

    d[7] = s[7] + s[6]
    v += a[7] * d[7]

    let yNew = (v.sign == .minus) ? -1.0 : 1.0
    idx = currentIdx ^ 1
    prevY = yNew
    return yNew
  }

  @inlinable
  @inline(__always)
  public func sdmSample(_ x: Double) -> Double {
    switch cachedOrder {
    case 4: return sdmSample4(x)
    case 5: return sdmSample5(x)
    case 6: return sdmSample6(x)
    case 7: return sdmSample7(x)
    case 8: return sdmSample8(x)
    default:
      let currentIdx = idx
      let s = nonTrellisState.advanced(by: currentIdx * 8)
      let d = nonTrellisState.advanced(by: (currentIdx ^ 1) * 8)
      let a = cachedA
      let g = cachedG
      let y = prevY

      d[0] = s[0] - g[0] * s[1] + x - y
      var v = x + a[0] * d[0]
      var i = 1
      while i < cachedOrder - 1 {
        d[i] = s[i] + s[i - 1] - g[i] * s[i + 1]
        v += a[i] * d[i]
        i += 1
      }
      d[i] = s[i] + s[i - 1]
      v += a[i] * d[i]

      let yNew = (v.sign == .minus) ? -1.0 : 1.0
      idx = currentIdx ^ 1
      prevY = yNew
      return yNew
    }
  }
}
