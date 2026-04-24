use realfft::{RealFftPlanner, RealToComplex};
use std::sync::Arc;

pub struct SpectrumAnalyzer {
    buffer: Vec<f32>,
    write_pos: usize,
    capacity: usize,
    center_frequencies: Vec<f32>,
    cached_samplerate: u32,
    cached_chunksize: usize,
    cached_fft_n: usize,
    cached_bins: Vec<(usize, usize)>,
    cached_window: Vec<f32>,
    cached_fft: Option<Arc<dyn RealToComplex<f32>>>,
    pub generation: u64,
}

impl SpectrumAnalyzer {
    pub fn new() -> Self {
        let center_frequencies = vec![
            25.0, 31.5, 40.0, 50.0, 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0,
            500.0, 630.0, 800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0, 4000.0, 5000.0,
            6300.0, 8000.0, 10000.0, 12500.0, 16000.0, 20000.0,
        ];
        let capacity = 32768; // Power of 2 for efficiency
        Self {
            buffer: vec![0.0; capacity],
            write_pos: 0,
            capacity,
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

    pub fn add_samples(&mut self, samples: &[f32], generation: u64) {
        if generation != self.generation {
            return;
        }
        // Zero-allocation push to fixed ring buffer
        for &s in samples {
            self.buffer[self.write_pos] = s;
            self.write_pos = (self.write_pos + 1) % self.capacity;
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

        // Clamp FFT size to buffer capacity
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

        let mut window = vec![0.0f32; fft_n];
        for (i, val) in window.iter_mut().enumerate().take(fft_n) {
            *val = 0.5 * (1.0 - (2.0 * std::f32::consts::PI * i as f32 / (fft_n - 1) as f32).cos());
        }

        let mut planner = RealFftPlanner::<f32>::new();
        self.cached_fft = Some(planner.plan_fft_forward(fft_n));
        self.cached_fft_n = fft_n;
        self.cached_bins = band_bins;
        self.cached_window = window;
    }

    pub fn compute_spectrum(&mut self) -> Vec<f32> {
        if self.cached_samplerate == 0 || self.cached_fft_n == 0 || self.cached_fft.is_none() {
            return vec![-100.0; 30];
        }

        let fft_n = self.cached_fft_n;
        let mut input = vec![0.0f32; fft_n];

        // Reconstruct contiguous window from circular buffer
        for (i, val) in input.iter_mut().enumerate().take(fft_n) {
            // Index logic: write_pos is the NEXT write position,
            // so write_pos - fft_n + i is the correct sequence.
            let idx = (self.write_pos + self.capacity - fft_n + i) % self.capacity;
            *val = self.buffer[idx] * self.cached_window[i];
        }

        let fft = self.cached_fft.as_ref().unwrap().clone();
        let mut output = fft.make_output_vec();
        if fft.process(&mut input, &mut output).is_err() {
            return vec![-100.0; 30];
        }

        let norm_scale = 4.0 / fft_n as f32;
        let to_db = |linear: f32| {
            if linear <= 0.0000000001f32 {
                -100.0f32
            } else {
                20.0f32 * linear.log10()
            }
        };

        let mut new_bands = vec![-100.0f32; 30];
        for (i, &(lo, hi)) in self.cached_bins.iter().enumerate() {
            let mut peak_mag = 0.0f32;
            for bin in lo..=hi {
                if bin < output.len() {
                    let mag = output[bin].norm() * norm_scale;
                    if mag > peak_mag {
                        peak_mag = mag;
                    }
                }
            }
            new_bands[i] = to_db(peak_mag);
        }

        new_bands
    }
}
