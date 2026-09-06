# SpectralCore Accuracy Validation Methodology

## Overview

This document describes the numerical accuracy validation methodology for the SpectralCore DSP engine. The validation suite proves correctness through deterministic testing of mathematical properties rather than visual inspection.

## Test Architecture

### Test Categories

1. **Window Function Validation** - Coherent gain accuracy for all supported windows
2. **dB Conversion Accuracy** - Power/magnitude to dB with floor handling
3. **Frequency/Bin Mathematics** - Bin↔frequency conversion correctness
4. **Silence/Zero Handling** - Proper handling of zero-valued signals
5. **Multi-Rate/Fft-Size/Window Combinations** - Cross-configuration consistency

### Test Configuration

```cpp
SAMPLE_RATES = [16000, 22050, 44100, 48000]
FFT_SIZES = [256, 512, 1024, 2048]
WINDOWS = ["rectangular", "hann", "hamming", "blackman"]
```

### Edge Cases Tested

| Edge Case | Description | Expected Behavior |
|-----------|-------------|-------------------|
| DC (0 Hz) | Zero frequency component | Bin 0, frequency 0 Hz |
| Nyquist | Exactly fs/2 | Bin N/2, frequency fs/2 |
| Near Nyquist | Just below fs/2 | Correct bin mapping |
| Low Amplitude | 0.001 amplitude | Non-zero but small output |
| Silence | All zeros | Zero magnitude everywhere |
| Clipping | Amplitude > 1.0 | Handled by dB floor |
| Off-Bin Frequencies | Not at bin centers | Correct bin estimation within tolerance |
| Multiple Tones | Multiple simultaneous sines | Multiple peaks at correct bins |

## Documented Tolerances

| Metric | Tolerance | Justification |
|--------|-----------|---------------|
| Frequency (on-bin) | 0.5 bins | DFT resolution limit; peak must be closer to correct bin than neighbors |
| Frequency (off-bin) | 1.5 bins | Spectral leakage spreads peak; raw peak finding limited |
| Amplitude | 0.02 linear (2%) | Window coherent gain normalization error bound |
| Power dB | 0.5 dB | 10*log10 power conversion; ~1% magnitude error at high SNR |
| Magnitude dB | 1.0 dB | 20*log10 magnitude conversion; JND for spectral visualization |

### Tolerance Derivation

All tolerances are derived from DSP theory, NOT empirical fitting:

1. **Frequency on-bin (0.5 bins)**: For a signal exactly at bin center, the DFT peak must be at that bin. Error < 0.5 bins guarantees the peak is closer to the correct bin than any neighbor. This is the fundamental DFT resolution limit.

2. **Amplitude (0.02 linear)**: Window coherent gain normalization. Hann window has coherent gain = 0.5 exactly. Normalization error directly maps to amplitude error. 2% bound covers floating-point accumulation error.

3. **Power dB (0.5 dB)**: 10*log10(power) conversion. At high SNR, 0.5 dB corresponds to ~1% magnitude error. At low SNR, the -90 dB floor dominates.

4. **Magnitude dB (1.0 dB)**: 20*log10(mag) conversion. 1.0 dB is the approximate just-noticeable difference for spectral visualization applications.

## Machine-Readable Results

No JSON results file is emitted. Results are the ctest verdict plus the
measured maxima printed by `dsp_accuracy` (`max_abs_fft_error`,
`max_abs_roundtrip_error`). Do not reintroduce a results.json writer
without a consumer for it.

## Sanitizer-Enabled Test Configuration (actual)

Sanitizer targets live in `CMakeLists.txt`, not in a separate workflow:

- `test_fft_asan` (ASan+UBSan) and `test_fft_msan` (Clang-only) configure
  on non-MSVC toolchains only.
- `test_fft_asan_msvc` (`/fsanitize=address`) runs on MSVC and executes
  in CI as part of the normal `ctest` run. The CI workflow is
  Windows-only; there is no Linux accuracy workflow.

## Known Limitations

### FFT Peak Detection (RESOLVED in S4)

The old forward-only FFT had no true inverse and peak tests were skipped.
The inverse is now the conjugation method and peak location is tested
exactly (`test_fft` Test 6, `dsp_accuracy` known-signal suite).

### Floating-Point Precision

All production calculations use `float` (32-bit) precision. Independent
double-precision verification lives in `tests/dsp/test_dsp_accuracy.cpp`
(naive O(N^2) DFT oracle); the former Python validator was removed in S4
because it imported production code as its own reference.

## Running the Tests

```bash
# Build all targets including accuracy tests
cmake -B build && cmake --build build --config Debug

# Run C++ accuracy tests
./build/Debug/test_fft.exe

# Run existing unit tests
ctest --test-dir build -C Debug --output-on-failure

# Run with AddressSanitizer (Linux/macOS)
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -fsanitize=undefined" -B build_asan
cmake --build build_asan --target test_fft
./build_asan/test_fft
```

## Future Optimization Guard

**A future optimization must not silently change established DSP behavior.**

Any changes to the DSP engine must:
1. Pass all accuracy validation tests
2. Not increase numerical errors beyond documented tolerances
3. Maintain identical output for deterministic test signals
4. Update this document if tolerances change

The accuracy validation suite serves as a regression gate for all future DSP modifications.