// SIMD2<Double> load/store helpers used by the FFT engines. These pin
// Swift's compiler into emitting NEON 128-bit `ldur q` / `stur q` plus
// `fmul.2d`, `fmla.2d`, `fadd.2d`, etc. — the natural
// `SIMD2<Double>(arr[k], arr[k+1])` and `arr[i] = v.x; arr[i+1] = v.y`
// forms scalarize through `d` registers (verified via `otool -tvV`:
// 0 vector ops vs 1006 scalar). Routing through `loadUnaligned` /
// `storeBytes` keeps both lanes resident in `q` registers across the
// full butterfly.
@inline(__always)
func ldSIMD2(_ p: UnsafePointer<Double>, _ idx: Int) -> SIMD2<Double> {
  return UnsafeRawPointer(p + idx).loadUnaligned(as: SIMD2<Double>.self)
}

@inline(__always)
func stSIMD2(_ p: UnsafeMutablePointer<Double>, _ idx: Int, _ v: SIMD2<Double>) {
  UnsafeMutableRawPointer(p + idx).storeBytes(of: v, as: SIMD2<Double>.self)
}
