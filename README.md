# CamillaDSP Monitor

A beautiful, high-performance native macOS app to control and monitor your CamillaDSP workflow. It brings advanced digital signal processing features to your desktop with an intuitive interface and hyper-realistic visualizations.

## Screenshots

![Dashboard](Screenshot-Dashboard.png)
![Devices](Screenshot-Device.png)
![EQ](Screenshot-EQ.png)

## Requirements

- macOS 15+ (Sequoia)
- Swift 6.0+ (Strict Concurrency enabled)
- Rust toolchain (latest stable)

## Building

The project uses a unified `Makefile` to handle the multi-language build pipeline (Rust + UniFFI + Swift).

### Build and Package as macOS Application (.app)
This will compile the Rust bridge with native CPU optimizations (`-C target-cpu=native`), generate the Swift bindings, and package the final signed application:
```bash
make app          # Builds CamillaDSPMonitor.app in the root directory
make install      # Builds and copies to /Applications/
```

### Simple Build (Command Line)
```bash
make build        # Compiles everything without packaging
```

## What You Can Do

CamillaDSP Monitor empowers you to take full control of your audio experience with a suite of professional-grade DSP features, inspired by high-end audio hardware like the RME ADI-2 DAC.

### Visualize Your Sound
- **Hyper-Realistic VU Meters**: Watch your audio levels on calibrated RMS and Peak needles with warm amber illumination.
- **Precision Spectrum Analyzer**: See the frequency content of your audio in real-time across the human audible range (20 Hz to 20 kHz) using a fast Fourier transform (FFT) for outstanding musical visualization.
- **Spectrogram (Waterfall Plot)**: Track frequency history over time to identify sustained frequencies and resonances.
- **Vector Scope (Goniometer)**: Visualize the stereo image and phase relationships between left and right channels in real-time.

### Tailor Your Stereo Image
- **Stereo Width**: Adjust the stereo width from mono to extra-wide, or swap channels.
- **Mid/Side Processing**: Encode and decode mid/side signals to manipulate spatial information.
- **Phase Inversion**: Correct poorly mastered recordings by flipping the polarity of left, right, or both channels.

### Optimize for Headphones
- **Crossfeed**: Enjoy a more natural, speaker-like listening experience on headphones with 5 levels of customizable crossfeed.

### Precision Equalization
- **Visual EQ Editor**: Fine-tune your sound with an interactive frequency response graph. Drag handles to adjust bands directly.
- **AutoEq & CSV Support**: Import presets from AutoEq or EqualizerAPO to easily apply headphone or room corrections.
- **13 Filter Types**: Support for peaking, shelving, notch, and pass filters.

### Restore and Protect
- **Loudness Compensation**: Automatically boost bass and treble at lower volumes to match human hearing curves (Fletcher-Munson).
- **Emphasis Control**: Fix bright old CDs recorded with pre-emphasis by applying the correct de-emphasis filter.
- **DC Protection**: Protect your speakers and headphones from harmful DC signals with a zero-latency high-pass filter at 7 Hz.

### Stay Focused with Mini Player
- Keep an eye on your levels, spectrum, or vector scope with a floating, translucent overlay that stays above all windows, perfect for watching full-screen video while monitoring your audio.

## Acknowledgments

- [CamillaDSP](https://github.com/HEnquist/camilladsp) by Henrik Enquist
- Audio EQ Cookbook by Robert Bristow-Johnson — biquad coefficient formulas

## License

See [LICENSE](LICENSE).
