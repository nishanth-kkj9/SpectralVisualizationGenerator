# Phase 13: Multi-Resolution Spectral Analysis — Design Spec

## Overview

Implement multi-band STFT analysis: split frequency axis into bands, run separate STFTs with different window sizes per band, merge results. Compare against fixed STFT, short-window, and long-window analyses. Experimental only — not enabled by default.

## Approach: Multi-Band STFT

Split Nyquist range into three bands:

| Band | Frequency Range | FFT Size (N) | Window | Time Resolution | Frequency Resolution |
|------|----------------|--------------|--------|-----------------|---------------------|
| Low | 0–2 kHz | 4096 | Hann | ~46 ms | ~12 Hz |
| Mid | 2–8 kHz | 1024 | Hann | ~12 ms | ~48 Hz |
| High | 8 kHz–Nyquist | 256 | Hann | ~3 ms | ~192 Hz |

Band boundaries are hardcoded defaults relative to Nyquist frequency. Each band runs independently with its own FFT size, window, and hop size (hop = N/4 for 75% overlap).

## Merging Strategy

- Resample all bands onto the time grid of the largest FFT (N=4096, hop=1024)
- For each band, only its frequency range gets bins; outside that range, bins are zero
- Merged frame's `n_fft` = max band's FFT size (4096)
- Frequency bins assigned by band: low band occupies bins 0–(2kHz/sr*N), mid band next, high band last
- `band_count` field records how many bands contributed to the frame

## Data Model Changes

### `AnalysisMetadata` (spectral_dataset.h)

```cpp
std::string analysis_method = "stft";
// Values: "stft", "stft_reassigned", "multiband_stft"
```

Binary serialization version bump: `SPECTRAL_DATASET_VERSION = 2`. New field appended after `analyzer_version`.

### `SpectralFrame` (spectral_dataset.h)

```cpp
int band_count = 1;  // number of bands merged into this frame
```

For standard STFT, `band_count = 1`. For multi-band, `band_count = 3`.

### Serialization

`write_metadata()` / `read_metadata()` updated to handle version 2: reads `analysis_method` and `band_count` only when version >= 2. Version 1 files read without these fields (backward compatible).

## CLI Interface

### Flags

- `--multiband` — Run comparison only. Prints table of all four methods. Does NOT write multi-band output.
- `--multiband --write` — Run comparison AND write multi-band result to output file.

### Comparison Table (stdout)

```
Method          Time Res (s)   Freq Res (Hz)   Compute (ms)   RMS Diff
─────────────   ───────────    ─────────────   ────────────   ────────
Fixed (1024)    0.0232         43.06           12.3           —
Short (256)     0.0058         172.27          4.1            0.0312
Long (4096)     0.0930         10.77           45.6           0.0187
Multi-band      0.0232         10.77–172.27    52.8           0.0098
```

- Time Res: hop_size / sample_rate
- Freq Res: sample_rate / n_fft
- Compute: wall-clock STFT time (excluding I/O)
- RMS Diff: RMS of |multi_mag[k] - method_mag[k]| across all time-frequency points

## Implementation Order

1. **Metadata changes** — Add `analysis_method`, `band_count`, version bump, serialization
2. **Multi-band analysis** — `MultiBandAnalyzer` class: band splitting, per-band STFT, merging
3. **CLI integration** — `--multiband` flag, comparison table, conditional write
4. **Tests** — Numerical, comparison, regression
5. **Documentation** — `docs/phase13-multiband.md`

## Files Modified

- `src/core/spectral/spectral_dataset.h` — metadata fields, version bump
- `src/core/spectral/spectral_dataset.cpp` — serialization
- `src/core/spectral/multiband_analyzer.h` — **new** — MultiBandAnalyzer class
- `src/core/spectral/multiband_analyzer.cpp` — **new** — implementation
- `src/cli/main.cpp` — `--multiband` flag, comparison logic
- `tests/phase13/test_multiband.cpp` — **new** — numerical tests
- `tests/phase13/test_multiband_compare.cpp` — **new** — comparison tests
- `CMakeLists.txt` — new targets

## Error Handling

- If Nyquist < 8 kHz (sample rate < 16 kHz), multi-band degrades to 2 bands (low + mid) or 1 band with a warning
- If FFT size not supported (not power of 2), error and exit
- Band boundaries validated: each band must have at least 1 frequency bin

## What Is NOT Changed

- Standard STFT is the default. `--multiband` is opt-in.
- Existing tests pass without modification.
- No existing behavior changes when `--multiband` is not used.
