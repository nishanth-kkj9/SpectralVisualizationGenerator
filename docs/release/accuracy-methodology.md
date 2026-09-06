# Accuracy Methodology (release summary)

Full methodology: `tests/accuracy/ACCURACY_METHODOLOGY.md` (tolerances derived
from DSP theory: 0.5-bin on-bin frequency, 2% amplitude, 0.5 dB power,
1.0 dB magnitude).

Measured in this release (21/21 ctest pass, Release build):

- DSP accuracy suite (`test_fft` + accuracy tests): pass.
- Backend parity (`phase16`): CPU backend matches reference FFT/magnitude.
- GPU-vs-CPU rendering (`gpu_render`, 256×128, 4 interp×colormap combos):
  PSNR 61.8–66.5 dB (threshold 40 dB), SSIM 1.0000 (threshold 0.99).
- CLI/GUI equivalence: byte-identical PNGs for identical configs.
- Hardening suite (`hardening`, 25 checks): silence, clipping, 1e-4 amplitude,
  8 kHz / 96 kHz, corrupt/missing inputs — all fail clearly or render correctly.
- Sanitizer: MSVC AddressSanitizer smoke (`fft_asan_msvc`) passes.

No accuracy numbers beyond these measured results are claimed.
