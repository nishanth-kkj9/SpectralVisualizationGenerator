# Analysis Methods

- **STFT**: radix-2 Cooley-Tukey FFT (own implementation, 32-bit float),
  Hann/Hamming/Blackman/rectangular windows, configurable FFT/hop/overlap.
- **Mel (representation)**: STFT one-sided amplitude-corrected power
  aggregated through a triangular Mel filterbank
  (`mel(f) = 2595*log10(1+f/700)`) into explicit nonuniform bands
  (`--representation mel`; `--mel-bands`, `--mel-norm none|slaney|area`).
  Band centers/edges are explicit data, not FFT bins; phase and
  reassignment are not applicable. **Mel ≠ a logarithmic display axis**:
  the display scale only remaps an already computed dataset. Bark, ERB and
  CQT are display scales only — no filterbank representation exists for
  them yet.
- **Frequency scales** (display): linear, log, Mel, Bark, ERB, CQT.
- **Reassignment**: instantaneous-frequency + group-delay sharpening.
- **Multi-band**: fixed/short/long/multi comparison table (CLI `--multiband`).
- **Rendering**: CPU ThreadPool renderer; optional D3D11 compute-shader
  spectrogram path with CPU fallback (measured PSNR 61–66 dB, SSIM 1.0 vs CPU).
- **Color**: Viridis (256-entry LUT) or Heat; dB floor/ceiling normalization.
