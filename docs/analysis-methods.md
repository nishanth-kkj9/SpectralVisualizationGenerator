# Analysis Methods

- **STFT**: radix-2 Cooley-Tukey FFT (own implementation, 32-bit float),
  Hann/Hamming/Blackman/rectangular windows, configurable FFT/hop/overlap.
- **Frequency scales**: linear, log, Mel, Bark, ERB, CQT.
- **Reassignment**: instantaneous-frequency + group-delay sharpening.
- **Multi-band**: fixed/short/long/multi comparison table (CLI `--multiband`).
- **Rendering**: CPU ThreadPool renderer; optional D3D11 compute-shader
  spectrogram path with CPU fallback (measured PSNR 61–66 dB, SSIM 1.0 vs CPU).
- **Color**: Viridis (256-entry LUT) or Heat; dB floor/ceiling normalization.
