# Phase 13: Multi-Resolution Spectral Analysis

## Overview

Multi-band STFT splits the frequency axis into bands, each analyzed with a different FFT size. Low frequencies get large FFTs (fine frequency resolution), high frequencies get small FFTs (fine time resolution).

## Band Structure

| Band | Frequency Range | FFT Size | Time Resolution | Frequency Resolution |
|------|----------------|----------|-----------------|---------------------|
| Low | 0–2 kHz | 4096 | ~46 ms | ~12 Hz |
| Mid | 2–8 kHz | 1024 | ~12 ms | ~48 Hz |
| High | 8 kHz–Nyquist | 256 | ~3 ms | ~192 Hz |

## CLI Usage

    spectragen --multiband input.wav -o output.wav

The `--multiband` flag prints a comparison table of four analysis methods:

- Fixed STFT (N=1024)
- Short-window STFT (N=256)
- Long-window STFT (N=4096)
- Multi-band STFT (N=256+1024+4096)

## Data Model

`AnalysisMetadata` gains:

- `analysis_method`: `"stft"`, `"stft_reassigned"`, or `"multiband_stft"`
- `band_count`: number of bands merged per frame (1 for standard, 3 for multi-band)

Binary serialization version bumped to 2.

## Merging Strategy

Each band runs independently. Band outputs are mapped to the max-FFT-size frequency grid. Only the band's frequency range gets non-zero bins.

## Tests

| Test | Validates |
|------|-----------|
| `multiband_default_bands` | Band splitting produces correct ranges |
| `multiband_output_sizes` | Output vector sizes match configuration |
| `multiband_magnitudes_nonzero` | Non-zero input produces non-zero output |
| `multiband_band_count` | Band count metadata is correct |
| `multiband_time_resolution` | Time resolution is within bounds |
| `multiband_single_band` | Short/long window comparison |
| `multiband_fixed_stft` | Standard STFT method metadata |
| `compare_all_four_methods` | All four methods produce valid output |
| `compare_multiband_vs_fixed` | Multi-band vs fixed STFT comparison |
| `regression_standard_stft_unchanged` | Standard STFT output unchanged |
