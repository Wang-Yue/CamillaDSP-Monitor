// Reference-output generator for DSPMonitor's Swift resampler audit.
//
// Reads raw little-endian f64 samples (mono, one channel), runs a rubato
// resampler chosen via CLI arg, writes the resulting samples back as raw
// little-endian f64.
//
// Args: <mode> <input.raw> <output.raw> <fs_in> <fs_out> <chunk_size>
//       [--bench=N] [--no-partial]
//
// `--no-partial` skips the trailing partial-chunk emission, matching
// Swift's `runResampler` "complete chunks only" framing. Use it for
// apples-to-apples comparisons where partial-chunk discontinuities
// would otherwise inflate the residual RMS in stopband measurements.
//
// Modes (matching `ResamplerProfile` in Swift's `AsyncSincResampler`):
//   sinc-veryfast  : sincLen=64,  oversampling=1024, Hann2,           linear
//   sinc-fast      : sincLen=128, oversampling=1024, Blackman2,       linear
//   sinc-balanced  : sincLen=192, oversampling=512,  BlackmanHarris2, quadratic
//   sinc-accurate  : sincLen=256, oversampling=256,  BlackmanHarris2, cubic
//   poly-linear    : Async::new_poly, PolynomialDegree::Linear
//   poly-cubic     : Async::new_poly, PolynomialDegree::Cubic
//   poly-quintic   : Async::new_poly, PolynomialDegree::Quintic
//   poly-septic    : Async::new_poly, PolynomialDegree::Septic
//   fft            : Fft synchronous, FixedSync::Both, sub_chunks=1
//                    (matches Swift's SynchronousResampler — chunk_size is
//                    rounded up to the smallest multiple of input_rate/gcd
//                    on both sides)
//
// `--bench=N` disables the comparison output and instead runs the full
// process loop `N` times back-to-back (no reset between runs), measuring
// only the per-chunk hot path and printing
//   BENCH_NS_TOTAL=...  BENCH_OUT_FRAMES_PER_ITER=...  BENCH_ITERS=...
// to stderr. This is consumed by Swift's cross-language perf test.

use audioadapter_buffers::direct::SequentialSlice;
use rubato::{
    calculate_cutoff, Async, FixedAsync, FixedSync, Fft, Indexing, PolynomialDegree, Resampler,
    SincInterpolationParameters, SincInterpolationType, WindowFunction,
};
use std::convert::TryInto;
use std::env;
use std::fs::File;
use std::io::prelude::{Read, Seek, Write};
use std::io::{BufReader, BufWriter};
use std::time::Instant;

const BPS: usize = 8;

fn read_f64<R: Read + Seek>(r: &mut R) -> Vec<f64> {
    let mut buf = [0u8; BPS];
    let mut out = Vec::new();
    while r.read(&mut buf).unwrap() == BPS {
        out.push(f64::from_le_bytes(buf.as_slice().try_into().unwrap()));
    }
    out
}

fn write_f64<W: Write + Seek>(data: &[f64], w: &mut W) {
    for v in data {
        w.write_all(&v.to_le_bytes()).unwrap();
    }
}

fn make_resampler(
    mode: &str,
    ratio: f64,
    chunk_size: usize,
    fs_in: usize,
    fs_out: usize,
) -> Box<dyn Resampler<f64>> {
    // Helper to keep the four sinc profile arms tidy.
    fn make_sinc(
        ratio: f64,
        chunk_size: usize,
        sinc_len: usize,
        oversampling_factor: usize,
        window: WindowFunction,
        interpolation: SincInterpolationType,
    ) -> Box<dyn Resampler<f64>> {
        let f_cutoff = calculate_cutoff::<f32>(sinc_len, window);
        let params = SincInterpolationParameters {
            sinc_len,
            f_cutoff,
            oversampling_factor,
            interpolation,
            window,
        };
        Box::new(
            Async::<f64>::new_sinc(ratio, 1.1, &params, chunk_size, 1, FixedAsync::Input).unwrap(),
        )
    }

    match mode {
        "sinc-veryfast" => make_sinc(
            ratio, chunk_size, 64, 1024,
            WindowFunction::Hann2, SincInterpolationType::Linear),
        "sinc-fast" => make_sinc(
            ratio, chunk_size, 128, 1024,
            WindowFunction::Blackman2, SincInterpolationType::Linear),
        "sinc-balanced" => make_sinc(
            ratio, chunk_size, 192, 512,
            WindowFunction::BlackmanHarris2, SincInterpolationType::Quadratic),
        "sinc-accurate" => make_sinc(
            ratio, chunk_size, 256, 256,
            WindowFunction::BlackmanHarris2, SincInterpolationType::Cubic),
        "poly-linear" => Box::new(
            Async::<f64>::new_poly(
                ratio,
                1.1,
                PolynomialDegree::Linear,
                chunk_size,
                1,
                FixedAsync::Input,
            )
            .unwrap(),
        ),
        "poly-cubic" => Box::new(
            Async::<f64>::new_poly(
                ratio,
                1.1,
                PolynomialDegree::Cubic,
                chunk_size,
                1,
                FixedAsync::Input,
            )
            .unwrap(),
        ),
        "poly-quintic" => Box::new(
            Async::<f64>::new_poly(
                ratio,
                1.1,
                PolynomialDegree::Quintic,
                chunk_size,
                1,
                FixedAsync::Input,
            )
            .unwrap(),
        ),
        "poly-septic" => Box::new(
            Async::<f64>::new_poly(
                ratio,
                1.1,
                PolynomialDegree::Septic,
                chunk_size,
                1,
                FixedAsync::Input,
            )
            .unwrap(),
        ),
        "fft" => Box::new(Fft::<f64>::new(fs_in, fs_out, chunk_size, 1, 1, FixedSync::Both).unwrap()),
        other => panic!("unknown mode: {other}"),
    }
}

fn main() {
    let args: Vec<_> = env::args().collect();
    if args.len() < 7 {
        eprintln!(
            "usage: {} <mode> <input.raw> <output.raw> <fs_in> <fs_out> <chunk_size> [--bench=N]",
            args[0]
        );
        eprintln!(
            "modes: sinc-veryfast | sinc-fast | sinc-balanced | sinc-accurate \
             | poly-linear | poly-cubic | poly-quintic | poly-septic | fft"
        );
        std::process::exit(2);
    }
    let mode = &args[1];
    let in_path = &args[2];
    let out_path = &args[3];
    let fs_in: usize = args[4].parse().unwrap();
    let fs_out: usize = args[5].parse().unwrap();
    let chunk_size: usize = args[6].parse().unwrap();
    let bench_iters: usize = args
        .iter()
        .skip(7)
        .find_map(|a| a.strip_prefix("--bench="))
        .map(|n| n.parse().expect("--bench= expects an integer"))
        .unwrap_or(0);
    let no_partial: bool = args.iter().skip(7).any(|a| a == "--no-partial");

    let mut reader = BufReader::new(File::open(in_path).expect("open input"));
    let indata = read_f64(&mut reader);
    let nbr_in = indata.len();

    let ratio = fs_out as f64 / fs_in as f64;
    let mut resampler = make_resampler(mode, ratio, chunk_size, fs_in, fs_out);

    // Pre-allocate output with slack.
    let max_out_per_chunk = ((chunk_size as f64) * ratio).ceil() as usize + 64;
    let nbr_chunks_full = nbr_in / chunk_size;
    let total_out_capacity = (nbr_chunks_full + 4) * max_out_per_chunk;
    let mut outdata = vec![0f64; total_out_capacity];

    let in_adapter = SequentialSlice::new(&indata, 1, nbr_in).unwrap();
    let mut out_adapter = SequentialSlice::new_mut(&mut outdata, 1, total_out_capacity).unwrap();

    let mut indexing = Indexing {
        input_offset: 0,
        output_offset: 0,
        active_channels_mask: None,
        partial_len: None,
    };

    // One full sweep through the input. Reused for both the comparison and
    // the bench loop. Reset indexing each call so successive sweeps
    // overwrite the same output buffer.
    macro_rules! run_once {
        () => {{
            indexing.input_offset = 0;
            indexing.output_offset = 0;
            indexing.partial_len = None;
            let mut frames_left = nbr_in;
            let mut total_out: usize = 0;
            let mut next_in = resampler.input_frames_next();
            while frames_left >= next_in {
                let (n_in, n_out) = resampler
                    .process_into_buffer(&in_adapter, &mut out_adapter, Some(&indexing))
                    .expect("process");
                indexing.input_offset += n_in;
                indexing.output_offset += n_out;
                frames_left -= n_in;
                total_out += n_out;
                next_in = resampler.input_frames_next();
            }
            if frames_left > 0 && !no_partial {
                indexing.partial_len = Some(frames_left);
                let (_n_in, n_out) = resampler
                    .process_into_buffer(&in_adapter, &mut out_adapter, Some(&indexing))
                    .expect("process partial");
                total_out += n_out;
            }
            total_out
        }};
    }

    if bench_iters == 0 {
        // Default mode: produce reference output for the comparison test.
        let total_out = run_once!();
        let mut writer = BufWriter::new(File::create(out_path).expect("create output"));
        write_f64(&outdata[..total_out], &mut writer);
        eprintln!(
            "cdsp_resampler_compare[{mode}]: in={nbr_in} out={total_out} ratio={ratio:.6} delay={}",
            resampler.output_delay()
        );
    } else {
        // Bench mode: warm-up + timed loop. Skip the file write so it
        // doesn't pollute the timing.
        let total_out_per_iter = run_once!();
        let start = Instant::now();
        let mut total_out: usize = 0;
        for _ in 0..bench_iters {
            total_out = run_once!();
        }
        let elapsed_ns = start.elapsed().as_nanos();
        debug_assert_eq!(total_out, total_out_per_iter);
        eprintln!(
            "BENCH_NS_TOTAL={elapsed_ns}  BENCH_OUT_FRAMES_PER_ITER={total_out_per_iter}  BENCH_ITERS={bench_iters}  BENCH_MODE={mode}"
        );
    }
}
