# FFT + STFT Contract (S4)

## Representation and precision

Production DSP is `float` (`complex_f`, `float` magnitudes) for
performance. The independent test oracle is `double`. Never claim double
precision for production results. Test tolerances below derive from
float32 eps ≈ 1.2e-7 scaled by FFT error growth (~log2 N).

## FFT (`src/core/dsp/fft.h`)

- Forward: `X[k] = Σ x[n]·exp(−2πi·k·n/N)`, in-place, complex.
- Inverse: conjugation method (conjugate → forward → conjugate → 1/N),
  so `IFFT(FFT(x)) == x` up to rounding (measured ≤ 6.4e-7 abs, N ≤ 64).
- Sizes: powers of two, N ≥ 2 (`is_valid_fft_size`). Anything else is
  left untouched by `fft()`; `fft_checked()` reports false. N == 1 is
  deliberately rejected (degenerate for STFT use).
- Bins: 0 = DC (plain sum), N/2 = Nyquist (real for real input).
  Real-input symmetry `X[N−k] == conj(X[k])` holds up to float error.
- `fft()` itself applies **no** normalization; scaling lives in `stft.h`.
- One butterfly exists (`fft_butterfly_std`); two broken historical
  variants were deleted in S4.
- `CPUBackend::fft` forwards to `::fft` — an adapter, not a second
  implementation. No GPU FFT exists (`gpu_backend.h` throws).

## STFT (`src/core/dsp/stft.h`, the single production frame math)

- Parameters are explicit: sample rate, `n_fft`, hop, prebuilt window +
  its coherent gain. No hidden defaults in the math.
- Frame policy: `frames = floor((total − n_fft) / hop) + 1`, 0 when the
  input is shorter than one frame. Frame f starts at sample `f·hop`;
  timestamp = start / sample_rate, exact.
- Last-frame policy: partial trailing frames are dropped (documented;
  callers needing edge coverage zero-pad explicitly — none do today).
- Window placement: window[0] multiplies the first sample of the frame.

## Normalization (the documented convention)

For interior bins `0 < k < N/2`:

```text
mag[k] = |X[k]| / (N · cg) · 2      (one-sided, amplitude-corrected)
power  = mag²
phase  = atan2(im, re)
```

DC and Nyquist bins are single-sided by nature and are NOT doubled.
`cg` = `window_coherent_gain` (sum/N). Consequence: an exact-bin unit
sinusoid reads magnitude 1.0 independent of N and window (tested for
N ∈ {512, 2048, 8192} × {hann, hamming, blackman, rectangular} ±2%).

## magnitude / power / dB

- `magnitude = |X|`, `power = |X|²`, `power_to_db = 10·log10(p/ref)`,
  `magnitude_to_db = 20·log10(m/ref)`, floor −90 dB default.
- `magnitude_to_db(m) == power_to_db(m²)` up to float rounding (tested).
- Zero/negative/NaN inputs map to the floor — NaN never propagates.
  +Inf passes through; renderers clamp to the display ceiling.

## Windows

Symmetric convention (`N−1` denominator): Hann, Hamming, Blackman,
Rectangular. Coherent gains are exact rational values of N
(Hann `0.5·(1−1/N)`, Hamming `0.54−0.46/N`, Blackman `0.42−0.42/N`,
Rectangular `1.0`) — tested against independent double formulas.
Only implemented windows are exposed; Kaiser/Tukey/Blackman-Harris
do not exist in this codebase.

## Tolerances (§17 rationale)

| Check | Tolerance | Why appropriate |
|---|---|---|
| FFT vs double DFT (N ≤ 64) | abs 1e-4 | error ~1e-6; structural bugs err ~1.0 |
| Round trip, N ≤ 1024 | rel 1e-4 | roundoff ~1e-6 relative |
| Round trip, N > 1024 | rel 5e-3 | error grows with N; still 200× below defect level |
| Window formula | abs 1e-5 | dominated by float32-vs-float64 PI |
| Coherent gain | abs 1e-6 | exact rational identity |
| Unit-sine magnitude | rel 2% | mirror leakage for Hann-class windows |
| Peak bin location | exact | exact-bin signals, no ambiguity |
