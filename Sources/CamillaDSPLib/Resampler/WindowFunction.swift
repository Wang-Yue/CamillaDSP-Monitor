// CamillaDSP-Swift: Window functions + cutoff calculation for the windowed-sinc
// resampler kernel. Mirrors rubato's `windows.rs` exactly so Swift's filter
// kernel matches rubato's bit-for-bit.

import Foundation

/// Window functions usable for sinc-kernel design. The `*2` variants are the
/// squared versions of the periodic base window — wider main lobe but stronger
/// stopband attenuation. Mirrors rubato's `WindowFunction` enum.
enum WindowFunction {
  case hann
  case hann2
  case blackman
  case blackman2
  case blackmanHarris
  case blackmanHarris2
}

/// Periodic window value at sample index `i` of a length-`n` window.
/// Mirrors `windowfunctions::GenericWindowIter::calc_at_index` — each harmonic
/// is `cos(2k · π · i / n)` computed with the operand order
/// `((2k * π) * i) / n`, **not** chained off the first harmonic. Reproducing
/// that exact order is what makes the kernel bit-equivalent versus rubato.
@inline(__always)
func windowValue(_ window: WindowFunction, i: Int, n: Int) -> Double {
  let x = Double(i)
  let len = Double(n)
  // Match `(2k * PI * x_float / len_float).cos()` from windowfunctions 0.1.1.
  let arg2 = 2.0 * .pi * x / len
  let arg4 = 4.0 * .pi * x / len
  let arg6 = 6.0 * .pi * x / len
  switch window {
  case .hann:
    return 0.5 - 0.5 * cos(arg2)
  case .hann2:
    let w = 0.5 - 0.5 * cos(arg2)
    return w * w
  case .blackman:
    return 0.42 - 0.5 * cos(arg2) + 0.08 * cos(arg4)
  case .blackman2:
    let w = 0.42 - 0.5 * cos(arg2) + 0.08 * cos(arg4)
    return w * w
  case .blackmanHarris:
    return 0.35875 - 0.48829 * cos(arg2) + 0.14128 * cos(arg4) - 0.01168 * cos(arg6)
  case .blackmanHarris2:
    let w = 0.35875 - 0.48829 * cos(arg2) + 0.14128 * cos(arg4) - 0.01168 * cos(arg6)
    return w * w
  }
}

/// f32 cutoff calculation matching rubato's `calculate_cutoff::<f32>`. The
/// audio path runs in f64 but rubato computes the cutoff in f32 and then
/// coerces it up; we match that here so kernel-derived constants stay
/// bit-equivalent across resamplers.
func calculateCutoffF32(sincLen: Int, window: WindowFunction) -> Float {
  let (k1, k2, k3): (Float, Float, Float)
  switch window {
  case .blackmanHarris:
    (k1, k2, k3) = (
      Float(8.041443677716476), Float(55.9506779343387), Float(898.0287985384213)
    )
  case .blackmanHarris2:
    (k1, k2, k3) = (
      Float(13.745202940783823), Float(121.73532586374934), Float(5964.163279612051)
    )
  case .blackman:
    (k1, k2, k3) = (
      Float(6.159598046201173), Float(18.926415097606878), Float(653.4247430458968)
    )
  case .blackman2:
    (k1, k2, k3) = (
      Float(9.506235102129398), Float(79.13120634953742), Float(1502.2316160588925)
    )
  case .hann:
    (k1, k2, k3) = (
      Float(3.3481080887677166), Float(10.106519434875038), Float(78.96345249024414)
    )
  case .hann2:
    (k1, k2, k3) = (
      Float(5.38751148378734), Float(29.69451915489501), Float(184.82117462266237)
    )
  }
  let n = Float(sincLen)
  return 1.0 / (k1 / n + k2 / (n * n) + k3 / (n * n * n) + 1.0)
}

/// Calculate a suitable relative cutoff frequency for the given sinc length and
/// window. Mirrors rubato's `calculate_cutoff` (`windows.rs:88-131`) — a cubic
/// fit `1 / (k1/n + k2/n² + k3/n³ + 1)` calibrated per window.
func calculateCutoff(sincLen: Int, window: WindowFunction) -> Double {
  let (k1, k2, k3): (Double, Double, Double)
  switch window {
  case .blackmanHarris:
    (k1, k2, k3) = (8.041443677716476, 55.9506779343387, 898.0287985384213)
  case .blackmanHarris2:
    (k1, k2, k3) = (13.745202940783823, 121.73532586374934, 5964.163279612051)
  case .blackman:
    (k1, k2, k3) = (6.159598046201173, 18.926415097606878, 653.4247430458968)
  case .blackman2:
    (k1, k2, k3) = (9.506235102129398, 79.13120634953742, 1502.2316160588925)
  case .hann:
    (k1, k2, k3) = (3.3481080887677166, 10.106519434875038, 78.96345249024414)
  case .hann2:
    (k1, k2, k3) = (5.38751148378734, 29.69451915489501, 184.82117462266237)
  }
  let n = Double(sincLen)
  return 1.0 / (k1 / n + k2 / (n * n) + k3 / (n * n * n) + 1.0)
}

/// Build the windowed-sinc table the same way rubato does (`sinc.rs:17-49`):
///   1. Compute `y[i] = window[i] * sinc((i - totpoints/2) * fc / factor)` for
///      i ∈ [0, totpoints) using the periodic window.
///   2. Sum y, divide by `factor`.
///   3. Decimate: `sincs[factor - n - 1][p] = y[factor*p + n] / norm`.
/// Stored layout: `table[s * sincLen + p] == sincs[s][p]`.
func makeSincTable(sincLen: Int, oversamplingFactor: Int, window: WindowFunction, fc: Double)
  -> [Double]
{
  let totpoints = sincLen * oversamplingFactor
  var y = [Double](repeating: 0, count: totpoints)
  for i in 0..<totpoints {
    let centred = Double(i) - Double(totpoints / 2)
    let xScaled = centred * fc / Double(oversamplingFactor)
    // Match rubato's `sinc(x) = (x * PI).sin() / (x * PI)` — argument order
    // matters for the (rare) f64 ULP differences this avoids.
    let arg = xScaled * .pi
    let sinc: Double = abs(xScaled) < 1e-10 ? 1.0 : sin(arg) / arg
    y[i] = sinc * windowValue(window, i: i, n: totpoints)
  }
  var ySum: Double = 0
  for v in y { ySum += v }
  let norm = ySum / Double(oversamplingFactor)

  var table = [Double](repeating: 0, count: totpoints)
  for p in 0..<sincLen {
    for n in 0..<oversamplingFactor {
      let s = oversamplingFactor - n - 1
      table[s * sincLen + p] = y[oversamplingFactor * p + n] / norm
    }
  }
  return table
}
