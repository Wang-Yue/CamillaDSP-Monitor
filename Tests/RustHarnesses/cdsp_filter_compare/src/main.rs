// Reference-output generator for CamillaDSP-Monitor's Swift filter audit.
//
// Reads raw little-endian f64 samples (mono), runs a camilladsp filter chosen
// via CLI arg, writes the resulting samples back as raw little-endian f64.
//
// Modes:
//   biquad   <a1> <a2> <b0> <b1> <b2> <samplerate> <chunk_size> <input> <output>
//   gain     <gain_db> <inverted:0|1> <mute:0|1> <chunk_size> <input> <output>
//   volume   <current_volume_db> <mute:0|1> <samplerate> <chunk_size> <input> <output>
//            (ramp_time_ms is hardcoded to 0 — both ends use instant gain
//             update so the test can compare bit-for-bit without ramp state.)
//   loudness <current_volume_db> <reference_level_db> <high_boost_db> <low_boost_db>
//            <attenuate_mid:0|1> <samplerate> <chunk_size> <input> <output>

use camillalib::config::{LoudnessFader, LoudnessParameters};
use camillalib::filters::basicfilters::{Gain, Volume};
use camillalib::filters::biquad::{Biquad, BiquadCoefficients};
use camillalib::filters::loudness::Loudness;
use camillalib::filters::Filter;
use camillalib::ProcessingParameters;
use std::sync::Arc;

use std::convert::TryInto;
use std::fs::File;
use std::io::{BufReader, BufWriter, Read, Write};

const BPS: usize = 8;

fn read_f64(path: &str) -> Vec<f64> {
    let mut buf = [0u8; BPS];
    let mut out = Vec::new();
    let mut r = BufReader::new(File::open(path).expect("open input"));
    while r.read(&mut buf).unwrap() == BPS {
        out.push(f64::from_le_bytes(buf.as_slice().try_into().unwrap()));
    }
    out
}

fn write_f64(path: &str, data: &[f64]) {
    let mut w = BufWriter::new(File::create(path).expect("create output"));
    for v in data {
        w.write_all(&v.to_le_bytes()).unwrap();
    }
}

fn run_filter<F: Filter + ?Sized>(filter: &mut F, input: Vec<f64>, chunk_size: usize) -> Vec<f64> {
    let mut samples = input;
    for chunk in samples.chunks_mut(chunk_size) {
        filter.process_waveform(chunk).expect("process_waveform");
    }
    samples
}

fn main() {
    let args: Vec<_> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: {} <mode> ... — see source for mode args", args[0]);
        std::process::exit(2);
    }
    match args[1].as_str() {
        "biquad" => {
            // <a1> <a2> <b0> <b1> <b2> <samplerate> <chunk_size> <input> <output>
            assert!(args.len() == 11, "biquad needs 9 trailing args");
            let a1: f64 = args[2].parse().unwrap();
            let a2: f64 = args[3].parse().unwrap();
            let b0: f64 = args[4].parse().unwrap();
            let b1: f64 = args[5].parse().unwrap();
            let b2: f64 = args[6].parse().unwrap();
            let samplerate: usize = args[7].parse().unwrap();
            let chunk_size: usize = args[8].parse().unwrap();
            let in_path = &args[9];
            let out_path = &args[10];
            let coeffs = BiquadCoefficients::new(a1, a2, b0, b1, b2);
            let mut filter = Biquad::new("test", samplerate, coeffs);
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "gain" => {
            // <gain_db> <inverted:0|1> <mute:0|1> <chunk_size> <input> <output>
            assert!(args.len() == 8, "gain needs 6 trailing args");
            let gain_db: f64 = args[2].parse().unwrap();
            let inverted: bool = args[3] == "1";
            let mute: bool = args[4] == "1";
            let chunk_size: usize = args[5].parse().unwrap();
            let in_path = &args[6];
            let out_path = &args[7];
            let mut filter = Gain::new("test", gain_db, inverted, mute, false);
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "volume" => {
            // <current_volume_db> <mute:0|1> <samplerate> <chunk_size> <input> <output>
            // ramp_time_ms is forced to 0 so the filter takes the constant-gain
            // branch on every chunk — this lets the Swift comparison test
            // ignore ramp state.
            assert!(args.len() == 8, "volume needs 6 trailing args");
            let current_volume: f32 = args[2].parse().unwrap();
            let mute: bool = args[3] == "1";
            let samplerate: usize = args[4].parse().unwrap();
            let chunk_size: usize = args[5].parse().unwrap();
            let in_path = &args[6];
            let out_path = &args[7];
            let processing_params = Arc::new(ProcessingParameters::new(
                &[current_volume, 0.0, 0.0, 0.0, 0.0],
                &[mute, false, false, false, false],
            ));
            let mut filter = Volume::new(
                "test",
                /*ramp_time_ms=*/ 0.0,
                /*limit=*/ 50.0,
                current_volume,
                mute,
                chunk_size,
                samplerate,
                processing_params,
                /*fader=*/ 0,
            );
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "loudness" => {
            // <current_volume_db> <reference_level_db> <high_boost_db>
            //   <low_boost_db> <attenuate_mid:0|1> <samplerate> <chunk_size>
            //   <input> <output>
            assert!(args.len() == 11, "loudness needs 9 trailing args");
            let current_volume: f32 = args[2].parse().unwrap();
            let reference_level: f32 = args[3].parse().unwrap();
            let high_boost: f32 = args[4].parse().unwrap();
            let low_boost: f32 = args[5].parse().unwrap();
            let attenuate_mid: bool = args[6] == "1";
            let samplerate: usize = args[7].parse().unwrap();
            let chunk_size: usize = args[8].parse().unwrap();
            let in_path = &args[9];
            let out_path = &args[10];
            let processing_params = Arc::new(ProcessingParameters::new(
                &[current_volume, 0.0, 0.0, 0.0, 0.0],
                &[false, false, false, false, false],
            ));
            // Make sure current_volume on the shared params reflects what we
            // pass in — Loudness::process_waveform reads
            // processing_params.current_volume(fader), not target.
            processing_params.set_current_volume(0, current_volume);
            let conf = LoudnessParameters {
                reference_level,
                high_boost: Some(high_boost),
                low_boost: Some(low_boost),
                fader: Some(LoudnessFader::Main),
                attenuate_mid: Some(attenuate_mid),
            };
            let mut filter = Loudness::from_config("test", conf, samplerate, processing_params);
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        other => {
            eprintln!("unknown mode: {other}");
            std::process::exit(2);
        }
    }
}
