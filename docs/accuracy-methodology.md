# Accuracy Methodology (release summary)

Full methodology: `tests/accuracy/ACCURACY_METHODOLOGY.md` (tolerances derived
from DSP theory: 0.5-bin on-bin frequency, 2% amplitude, 0.5 dB power,
1.0 dB magnitude).

Measured in this release (24/24 ctest pass, Release build):

- DSP accuracy suite (`test_fft` incl. exact-bin peak + `dsp_accuracy`
  241 checks vs independent double DFT oracle): pass.
  `max_abs_fft_error = 2.01e-06`, `max_abs_roundtrip_error = 6.4e-07`.
  Exact-bin unit sine reads magnitude 1.0 ±2% across N and windows
  (amplitude-corrected one-sided STFT; see `docs/dsp/fft-stft.md`).
- Backend parity (`phase16`): CPU backend matches reference FFT/magnitude.
- GPU-vs-CPU rendering (`gpu_render`, 256×128, 4 interp×colormap combos):
  PSNR 61.8–66.5 dB (threshold 40 dB), SSIM 1.0000 (threshold 0.99).
- CLI/GUI equivalence: byte-identical PNGs for identical configs.
- Hardening suite (`hardening`, 25 checks): silence, clipping, 1e-4 amplitude,
  8 kHz / 96 kHz, corrupt/missing inputs — all fail clearly or render correctly.
- Sanitizer: MSVC AddressSanitizer smoke (`fft_asan_msvc`) passes.

No accuracy numbers beyond these measured results are claimed.
