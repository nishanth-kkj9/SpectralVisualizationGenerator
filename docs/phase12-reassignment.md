# Phase 12: Time-Frequency Reassignment

## Overview

Time-Frequency Reassignment is a post-processing technique that sharpens spectrograms by moving energy from the standard STFT time-frequency grid to the true instantaneous time and frequency of each signal component.

Based on: Auger & Flandrin, "Improving the readability of time-frequency representations," IEEE Trans. Signal Processing, vol. 43, no. 5, pp. 1068–1089, May 1995.

## Mathematical Formulation

### Standard STFT

The Short-Time Fourier Transform of signal x(n) with window w(n):

    X(m,k) = Σ_n x(n) · w(n - m·h) · e^{-j·2π·k·n/N}

where m is the frame index, h is the hop size, k is the frequency bin, and N is the FFT size.

### Reassignment Operators

**Instantaneous Frequency (frequency reassignment):**

    ω̂(m,k) = k·(sr/N) + Im{X*(m,k) · X_{g'}(m,k)} / (2π·|X(m,k)|²) · sr

where X_{g'} = FFT{w'(n) · x(n)} is the STFT computed with the window derivative w'(n).

For a Hann window: w'(n) = π/(N-1) · sin(2π·n/(N-1))

**Group Delay (time reassignment):**

    τ̂(m,k) = Re{X*(m,k) · X_τ(m,k)} / |X(m,k)|² / sr

where X_τ = FFT{n · w(n) · x(n)} is the group-delay-weighted STFT.

### Properties

- **Exact for chirps**: Reassignment is exact for linear chirps (FM signals with linear instantaneous frequency).
- **Symmetric windows**: For symmetric windows, the frequency correction vanishes for pure tones (ω̂ = f₀ exactly).
- **Energy preservation**: Total energy is conserved; energy is only redistributed, not created or destroyed.

## Implementation

### CLI Flag

    spectragen --reassigned input.wav -o output.wav

### Data Model

`Spectral::SpectralFrame` gains two optional fields:

    std::vector<float> reassigned_times;   // seconds per bin
    std::vector<float> reassigned_freqs;   // Hz per bin

### Renderer

The spectrogram renderer uses a scatter mode when `reassigned_freqs` is non-empty:

- For each bin, the pixel position is determined by (reassigned_time, reassigned_freq) instead of (frame_time, bin_freq).
- A Gaussian anti-aliasing kernel distributes energy to the 4 nearest pixels.
- Falls back to standard gather mode when reassignment data is absent.

### FFT Bug Fix

This phase also fixed a latent bug in the FFT twiddle factor computation (`fft.h:81`). The twiddle factor at stage with butterfly length `len` was computed as `ftwiddle(N, j, len/2)` instead of the correct `ftwiddle(N, j, N/len)`. This affected all FFT-dependent computations.

## Tests

| Test | Validates |
|------|-----------|
| `test_impulse_group_delay` | Group delay at window center for impulse |
| `test_pure_tone_frequency` | Reassigned frequency matches true tone frequency |
| `test_chirp_range` | Chirp spans correct frequency range after reassignment |
| `test_sizes` | Output vector sizes match FFT configuration |
| `test_silent_frame` | Silent frames produce zero reassignment |
| `test_regression_conventional` | Conventional spectrogram unchanged by reassignment code |
| `test_regression_reassigned` | Reassigned spectrogram renders successfully |
| `test_regression_conventional_still_works` | Conventional mode unaffected by reassignment feature |
