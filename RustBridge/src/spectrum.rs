use num_complex::Complex;
use realfft::{RealFftPlanner, RealToComplex};
use std::sync::Arc;

pub struct SpectrumAnalyzer {
    // Circular buffer for incoming samples
    buffer: Vec<f32>,
    write_pos: usize,
    capacity: usize,

    // Pre-allocated buffers
    fft_input: Vec<f32>,
    fft_output: Vec<Complex<f32>>,

    center_frequencies: Vec<f32>,
    cached_samplerate: u32,
    cached_chunksize: usize,
    cached_fft_n: usize,
    cached_bins: Vec<(usize, usize)>,
    cached_window: Vec<f32>,
    cached_fft: Option<Arc<dyn RealToComplex<f32>>>,
    pub generation: u64,
}

type FftSnapshot = (
    Vec<f32>,
    Vec<f32>,
    Arc<dyn RealToComplex<f32>>,
    Vec<(usize, usize)>,
);

impl SpectrumAnalyzer {
    pub fn new() -> Self {
        let center_frequencies = vec![
            25.0, 31.5, 40.0, 50.0, 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0,
            500.0, 630.0, 800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0, 4000.0, 5000.0,
            6300.0, 8000.0, 10000.0, 12500.0, 16000.0, 20000.0,
        ];
        let capacity = 65536;
        Self {
            buffer: vec![0.0; capacity],
            write_pos: 0,
            capacity,
            fft_input: Vec::new(),
            fft_output: Vec::new(),
            center_frequencies,
            cached_samplerate: 0,
            cached_chunksize: 0,
            cached_fft_n: 0,
            cached_bins: Vec::new(),
            cached_window: Vec::new(),
            cached_fft: None,
            generation: 0,
        }
    }

    pub fn reset(&mut self, samplerate: u32, chunk_size: usize) {
        self.generation += 1;
        self.buffer.fill(0.0);
        self.write_pos = 0;
        self.update_cache(samplerate, chunk_size);
    }

    // High-performance sample addition. Must be fast.
    pub fn add_samples(&mut self, samples: &[f32], generation: u64) {
        if generation != self.generation || samples.is_empty() {
            return;
        }

        let n = samples.len();
        if n >= self.capacity {
            let start = n - self.capacity;
            self.buffer.copy_from_slice(&samples[start..]);
            self.write_pos = 0;
            return;
        }

        let first_part = n.min(self.capacity - self.write_pos);
        self.buffer[self.write_pos..self.write_pos + first_part]
            .copy_from_slice(&samples[..first_part]);

        if n > first_part {
            let second_part = n - first_part;
            self.buffer[..second_part].copy_from_slice(&samples[first_part..]);
            self.write_pos = second_part;
        } else {
            self.write_pos = (self.write_pos + first_part) % self.capacity;
        }
    }

    fn update_cache(&mut self, samplerate: u32, chunk_size: usize) {
        if samplerate == self.cached_samplerate
            && chunk_size == self.cached_chunksize
            && self.cached_fft_n > 0
        {
            return;
        }

        self.cached_samplerate = samplerate;
        self.cached_chunksize = chunk_size;

        if self.cached_samplerate == 0 || self.cached_chunksize == 0 {
            return;
        }

        let mut fft_n = 4096;
        if self.cached_samplerate < 16000 {
            fft_n = 2048;
        } else if self.cached_samplerate > 48000 {
            fft_n = 8192;
        } else if self.cached_samplerate > 96000 {
            fft_n = 16384;
        }

        while fft_n < self.cached_chunksize {
            fft_n *= 2;
        }

        let fft_n = fft_n.min(self.capacity);

        let bin_width = self.cached_samplerate as f32 / fft_n as f32;
        let factor = 2.0f32.powf(1.0 / 6.0);
        let mut band_bins = Vec::new();

        let half_n = fft_n / 2;
        for &freq in &self.center_frequencies {
            let f_lo = freq / factor;
            let f_hi = freq * factor;

            let bin_lo = (f_lo / bin_width) as usize;
            let bin_hi = (f_hi / bin_width) as usize;

            let lo = bin_lo.max(1).min(half_n - 1);
            let hi = bin_hi.max(lo).min(half_n - 1);
            band_bins.push((lo, hi));
        }

        // Use Blackman-Harris window for much better sideband suppression (-92dB)
        // compared to Hann (-32dB). This fixes "sidebands" around pure tones.
        let mut window = vec![0.0f32; fft_n];
        let a0 = 0.35875;
        let a1 = 0.48829;
        let a2 = 0.14128;
        let a3 = 0.01168;
        for (i, val) in window.iter_mut().enumerate() {
            let t = 2.0 * std::f32::consts::PI * i as f32 / (fft_n - 1) as f32;
            *val = a0 - a1 * t.cos() + a2 * (2.0 * t).cos() - a3 * (3.0 * t).cos();
        }

        let mut planner = RealFftPlanner::<f32>::new();
        let fft = planner.plan_fft_forward(fft_n);

        self.fft_input = vec![0.0f32; fft_n];
        self.fft_output = fft.make_output_vec();

        self.cached_fft = Some(fft);
        self.cached_fft_n = fft_n;
        self.cached_bins = band_bins;
        self.cached_window = window;
    }

    // This method now performs ONLY the data copy.
    // It returns the snapshot and the necessary state for FFT.
    pub fn get_snapshot(&self) -> Option<FftSnapshot> {
        if self.cached_fft_n == 0 || self.cached_fft.is_none() {
            return None;
        }

        let fft_n = self.cached_fft_n;
        let mut input = vec![0.0f32; fft_n];

        let first_part_len = fft_n.min(self.write_pos);
        if first_part_len < fft_n {
            let second_part_len = fft_n - first_part_len;
            let second_part_start = self.capacity - second_part_len;
            input[..second_part_len].copy_from_slice(&self.buffer[second_part_start..]);
            input[second_part_len..].copy_from_slice(&self.buffer[..first_part_len]);
        } else {
            let start = self.write_pos - fft_n;
            input.copy_from_slice(&self.buffer[start..self.write_pos]);
        }

        Some((
            input,
            self.cached_window.clone(),
            self.cached_fft.as_ref().unwrap().clone(),
            self.cached_bins.clone(),
        ))
    }

    // Static helper to process the snapshot outside the lock
    pub fn process_snapshot(
        mut input: Vec<f32>,
        window: &[f32],
        fft: Arc<dyn RealToComplex<f32>>,
        bins: &[(usize, usize)],
    ) -> Vec<f32> {
        let fft_n = input.len();

        // 1. Apply window
        for (i, val) in input.iter_mut().enumerate() {
            *val *= window[i];
        }

        // 2. FFT
        let mut output = fft.make_output_vec();
        if fft.process(&mut input, &mut output).is_err() {
            return vec![-100.0; 30];
        }

        // 3. Bands
        // Blackman-Harris coherent gain is ~0.35875.
        // Normalization scale: 2.0 (single-sided) / (fft_n * 0.35875)
        let norm_scale = 2.0 / (fft_n as f32 * 0.35875);
        let mut new_bands = vec![-100.0f32; 30];

        for (i, &(lo, hi)) in bins.iter().enumerate() {
            let mut peak_mag = 0.0f32;
            for bin in lo..=hi {
                if bin < output.len() {
                    let mag = output[bin].norm() * norm_scale;
                    if mag > peak_mag {
                        peak_mag = mag;
                    }
                }
            }

            new_bands[i] = if peak_mag <= 0.0000000001f32 {
                -100.0f32
            } else {
                20.0f32 * peak_mag.log10()
            };
        }

        new_bands
    }
}
