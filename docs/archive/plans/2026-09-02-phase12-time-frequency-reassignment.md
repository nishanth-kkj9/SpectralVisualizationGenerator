# Phase 12 — Time-Frequency Reassignment Implementation Plan

> **For agentic workers:** Use `superpowers:executing-plans` to implement this plan task-by-task.

**Goal:** Add time-frequency reassignment as an explicit analysis mode that improves time-frequency localization by reassigning STFT coefficients to the center of gravity of the signal's energy.

**Architecture:** Add reassignment computation to `ConcreteSpectralAnalyzer`. Store reassigned coordinates in `SpectralFrame` as optional vectors. Modify renderers to use reassigned coordinates when available.

**Tech Stack:** C++20, MSVC 19.51, CMake, no external deps.

**Spec:** `docs/superpowers/plans/2026-09-02-phase12-time-frequency-reassignment.md`

## Global Constraints

- MSVC 19.51, C++20, Windows
- No external deps
- All DSP/FFT self-contained in `src/core/dsp/`
- Existing tests must continue passing (11 suites)
- MSVC LSP errors are spurious — trust `cmake --build` output
- Preserve conventional STFT mode as baseline

---

### Task 1: Add reassigned coordinates to SpectralFrame

**Files:**
- Modify: `src/core/spectral/spectral_dataset.h`

**Interfaces:**
- Produces: `SpectralFrame::reassigned_times`, `SpectralFrame::reassigned_freqs`

**Changes:**
- Add `std::vector<float> reassigned_times` and `std::vector<float> reassigned_freqs` to `SpectralFrame`
- These store the reassigned time (group delay) and frequency (instantaneous frequency) for each bin
- When empty, standard STFT grid is used

- [ ] **Step 1: Add optional fields to SpectralFrame**

In `src/core/spectral/spectral_dataset.h`, add after line 236:

```cpp
    // Reassigned coordinates (empty for conventional STFT)
    std::vector<float> reassigned_times;   // group delay (seconds)
    std::vector<float> reassigned_freqs;   // instantaneous frequency (Hz)
```

- [ ] **Step 2: Update operator== to include new fields**

In `SpectralFrame::operator==`, add:

```cpp
        reassigned_times == other.reassigned_times &&
        reassigned_freqs == other.reassigned_freqs;
```

- [ ] **Step 3: Build to verify no errors**

Run: `cmake --build build --config Release`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add src/core/spectral/spectral_dataset.h
git commit -m "feat(spectral): add optional reassigned coordinates to SpectralFrame"
```

---

### Task 2: Implement reassignment computation in ConcreteSpectralAnalyzer

**Files:**
- Modify: `src/core/spectral/concrete_analyzer.h`
- Modify: `src/core/spectral/concrete_analyzer.cpp`

**Interfaces:**
- Produces: `ConcreteSpectralAnalyzer::set_reassignment(bool)`, `ConcreteSpectralAnalyzer::compute_reassigned_coordinates()`

**Changes:**
- Add `reassignment_enabled_` flag and `set_reassignment(bool)` method
- Implement `compute_reassigned_coordinates()` that computes instantaneous frequency and group delay
- Modify `analyze_segment()` to compute reassigned coordinates when enabled

**Mathematical Details:**
1. Compute standard STFT: X[k] = FFT{w[n]·x[n]}
2. Compute time-derivative STFT: X_t[k] = FFT{n·w[n]·x[n]}
3. Compute frequency-derivative STFT: X_f[k] = k·X[k]
4. For each bin k:
   - If |X[k]|² > threshold (1e-12):
     - Instantaneous frequency: ω̂[k] = (N/(2π·hop)) · Im{X*[k] · X_t[k]} / |X[k]|²
     - Group delay: τ̂[k] = Re{X*[k] · X_f[k]} / (2π · |X[k]|²)
   - Else: ω̂[k] = 0, τ̂[k] = 0

**Reference:** F. Auger and P. Flandrin, "Improving the readability of time-frequency and time-scale representations by the reassignment method," IEEE Trans. Signal Processing, vol. 43, no. 5, pp. 1068–1089, May 1995.

- [ ] **Step 1: Add reassignment flag to header**

In `src/core/spectral/concrete_analyzer.h`, add private member and method:

```cpp
    bool reassignment_enabled_{false};
    void compute_reassigned_coordinates(const std::vector<complex_f>& X,
                                        const float* samples, int n_fft,
                                        SpectralFrame& out);
```

Add public method:

```cpp
    void set_reassignment(bool enabled) { reassignment_enabled_ = enabled; }
```

- [ ] **Step 2: Implement compute_reassigned_coordinates**

In `src/core/spectral/concrete_analyzer.cpp`, add after `analyze_segment()`:

```cpp
void ConcreteSpectralAnalyzer::compute_reassigned_coordinates(
    const std::vector<complex_f>& X,
    const float* samples, int n_fft,
    SpectralFrame& out) {
    
    const int half = n_fft / 2 + 1;
    const float threshold = 1e-12f;
    
    // Compute time-derivative STFT: window with n * w[n]
    std::vector<complex_f> X_t(n_fft, complex_f(0.0f, 0.0f));
    for (int n = 0; n < n_fft; ++n) {
        X_t[n] = complex_f(static_cast<float>(n) * window_[n] * samples[n], 0.0f);
    }
    ::fft(X_t, false);
    
    // Compute frequency-derivative STFT: multiply by bin index
    std::vector<complex_f> X_f(n_fft, complex_f(0.0f, 0.0f));
    for (int k = 0; k < n_fft; ++k) {
        X_f[k] = complex_f(static_cast<float>(k) * X[k].real(),
                           static_cast<float>(k) * X[k].imag());
    }
    
    // Compute reassigned coordinates
    out.reassigned_times.resize(half);
    out.reassigned_freqs.resize(half);
    
    for (int k = 0; k < half; ++k) {
        float mag_sq = X[k].real() * X[k].real() + X[k].imag() * X[k].imag();
        
        if (mag_sq > threshold) {
            // Instantaneous frequency: Im{X* · X_t} / (2π · |X|²)
            complex_f conj_X = std::conj(X[k]);
            float dot_t = (conj_X * X_t[k]).imag();
            out.reassigned_freqs[k] = dot_t / (2.0f * PI * mag_sq) *
                                      static_cast<float>(sample_rate_) / static_cast<float>(n_fft);
            
            // Group delay: Re{X* · X_f} / (2π · |X|²)
            float dot_f = (conj_X * X_f[k]).real();
            out.reassigned_times[k] = dot_f / (2.0f * PI * mag_sq) /
                                      static_cast<float>(sample_rate_);
        } else {
            out.reassigned_times[k] = 0.0f;
            out.reassigned_freqs[k] = 0.0f;
        }
    }
}
```

- [ ] **Step 3: Modify analyze_segment to call reassignment**

In `ConcreteSpectralAnalyzer::analyze_segment()`, add before the return:

```cpp
    // Compute reassigned coordinates if enabled
    if (reassignment_enabled_) {
        compute_reassigned_coordinates(x, samples, n_fft, out);
    }
```

- [ ] **Step 4: Build to verify no errors**

Run: `cmake --build build --config Release`
Expected: Build succeeds

- [ ] **Step 5: Commit**

```bash
git add src/core/spectral/concrete_analyzer.h src/core/spectral/concrete_analyzer.cpp
git commit -m "feat(spectral): implement time-frequency reassignment in ConcreteSpectralAnalyzer"
```

---

### Task 3: Add --reassigned CLI flag

**Files:**
- Modify: `src/cli/main.cpp`

**Interfaces:**
- Produces: `CliConfig::reassigned`

**Changes:**
- Add `reassigned` boolean to `CliConfig`
- Add `--reassigned` flag parsing
- Wire to analyzer and renderers
- Update help text

- [ ] **Step 1: Add reassigned flag to CliConfig**

In `src/cli/main.cpp`, add to `CliConfig` struct:

```cpp
    bool reassigned = false;
```

- [ ] **Step 2: Add flag parsing**

In `parse_args()` function, add before the positional argument handling:

```cpp
        if (arg == "--reassigned") {
            cfg.reassigned = true;
            continue;
        }
```

- [ ] **Step 3: Update help text**

Add to help output:

```
  --reassigned    Use time-frequency reassignment for improved resolution
```

- [ ] **Step 4: Wire to analyzer**

In the analysis section, after creating the analyzer, add:

```cpp
    analyzer.set_reassignment(cfg.reassigned);
```

- [ ] **Step 5: Build to verify no errors**

Run: `cmake --build build --config Release`
Expected: Build succeeds

- [ ] **Step 6: Commit**

```bash
git add src/cli/main.cpp
git commit -m "feat(cli): add --reassigned flag for time-frequency reassignment"
```

---

### Task 4: Modify spectrogram renderer to use reassigned coordinates

**Files:**
- Modify: `src/core/rendering/spectrogram_renderer.cpp`

**Interfaces:**
- Consumes: `SpectralFrame::reassigned_times`, `SpectralFrame::reassigned_freqs`

**Changes:**
- When `reassigned_times` and `reassigned_freqs` are non-empty:
  - Use reassigned coordinates to place energy at the corrected positions
  - Instead of bin k → frequency k·sr/N, use reassigned_freqs[k]
  - Instead of frame t → time t·hop/sr, use reassigned_times[k]
- When empty, use standard STFT grid (existing behavior)

- [ ] **Step 1: Add reassigned coordinate lookup**

In the rendering loop, modify the frequency lookup to use reassigned coordinates when available:

```cpp
                // Use reassigned coordinates if available
                float actual_freq = freq;
                double actual_time = t0;
                
                if (!f0.reassigned_freqs.empty() && bin >= 0 && bin < Nk) {
                    actual_freq = f0.reassigned_freqs[bin];
                }
                if (!f0.reassigned_times.empty() && bin >= 0 && bin < Nk) {
                    actual_time = t0 + f0.reassigned_times[bin];
                }
```

- [ ] **Step 2: Build to verify no errors**

Run: `cmake --build build --config Release`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add src/core/rendering/spectrogram_renderer.cpp
git commit -m "feat(rendering): use reassigned coordinates in spectrogram renderer"
```

---

### Task 5: Add numerical tests

**Files:**
- Create: `tests/phase12/test_reassignment.cpp`
- Modify: `CMakeLists.txt`

**Tests:**
- Impulse signal: verify reassignment centers energy at t=0
- Pure tone: verify reassignment centers energy at correct frequency
- Chirp signal: verify reassignment follows time-varying frequency
- Numerical accuracy: verify derivatives are computed correctly

- [ ] **Step 1: Create test file**

Create `tests/phase12/test_reassignment.cpp`:

```cpp
// tests/phase12/test_reassignment.cpp
#include "concrete_analyzer.h"
#include "spectral_dataset.h"
#include <cstdio>
#include <cmath>
#include <vector>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

static void test_impulse_reassignment() {
    std::fprintf(stderr, "[test_impulse_reassignment]\n");
    const int n_fft = 1024;
    const int sr = 44100;
    
    // Create impulse at center
    std::vector<float> samples(n_fft, 0.0f);
    samples[n_fft / 2] = 1.0f;
    
    ConcreteSpectralAnalyzer analyzer;
    analyzer.set_fft_size(n_fft);
    analyzer.set_reassignment(true);
    
    SpectralFrame frame;
    bool ok = analyzer.analyze_frame(samples.data(), n_fft, frame);
    CHECK(ok, "impulse analysis succeeds");
    
    CHECK(!frame.reassigned_freqs.empty(), "reassigned freqs not empty");
    CHECK(!frame.reassigned_times.empty(), "reassigned times not empty");
    
    // Impulse should have energy at all frequencies, but reassigned time
    // should be near the impulse location (center of window)
    float mean_time = 0.0f;
    for (int k = 1; k < n_fft / 2; ++k) {
        mean_time += frame.reassigned_times[k];
    }
    mean_time /= (n_fft / 2 - 1);
    
    // Mean group delay should be near 0 (center of window)
    CHECK(std::abs(mean_time) < 0.001f, "mean group delay near zero");
}

static void test_pure_tone_reassignment() {
    std::fprintf(stderr, "[test_pure_tone_reassignment]\n");
    const int n_fft = 1024;
    const int sr = 44100;
    const float freq = 1000.0f;
    
    // Create pure tone
    std::vector<float> samples(n_fft);
    for (int n = 0; n < n_fft; ++n) {
        samples[n] = std::sin(2.0f * PI * freq * n / sr);
    }
    
    ConcreteSpectralAnalyzer analyzer;
    analyzer.set_fft_size(n_fft);
    analyzer.set_reassignment(true);
    
    SpectralFrame frame;
    bool ok = analyzer.analyze_frame(samples.data(), n_fft, frame);
    CHECK(ok, "pure tone analysis succeeds");
    
    // Find bin with maximum magnitude
    int max_bin = 0;
    float max_mag = 0.0f;
    for (int k = 1; k < n_fft / 2; ++k) {
        if (frame.magnitudes[k] > max_mag) {
            max_mag = frame.magnitudes[k];
            max_bin = k;
        }
    }
    
    // Reassigned frequency should be near 1000 Hz
    float reassigned_freq = frame.reassigned_freqs[max_bin];
    CHECK(std::abs(reassigned_freq - freq) < 50.0f, "reassigned frequency near 1000 Hz");
}

static void test_chirp_reassignment() {
    std::fprintf(stderr, "[test_chirp_reassignment]\n");
    const int n_fft = 1024;
    const int sr = 44100;
    
    // Create chirp from 500 Hz to 2000 Hz
    std::vector<float> samples(n_fft);
    for (int n = 0; n < n_fft; ++n) {
        float t = static_cast<float>(n) / sr;
        float f = 500.0f + 1500.0f * t;
        samples[n] = std::sin(2.0f * PI * f * t);
    }
    
    ConcreteSpectralAnalyzer analyzer;
    analyzer.set_fft_size(n_fft);
    analyzer.set_reassignment(true);
    
    SpectralFrame frame;
    bool ok = analyzer.analyze_frame(samples.data(), n_fft, frame);
    CHECK(ok, "chirp analysis succeeds");
    
    // Reassigned frequencies should span the chirp range
    float min_freq = 1e6f, max_freq = 0.0f;
    for (int k = 1; k < n_fft / 2; ++k) {
        if (frame.magnitudes[k] > 0.01f * frame.peak_magnitude) {
            min_freq = std::min(min_freq, frame.reassigned_freqs[k]);
            max_freq = std::max(max_freq, frame.reassigned_freqs[k]);
        }
    }
    
    CHECK(min_freq < 600.0f, "reassigned min freq near 500 Hz");
    CHECK(max_freq > 1900.0f, "reassigned max freq near 2000 Hz");
}

int main() {
    test_impulse_reassignment();
    test_pure_tone_reassignment();
    test_chirp_reassignment();
    std::fprintf(stderr, "\n=== reassignment: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt**

Add to `CMakeLists.txt`:

```cmake
# Phase 12 — Reassignment tests
add_executable(test_reassignment
    tests/phase12/test_reassignment.cpp
    src/core/spectral/concrete_analyzer.cpp
    src/core/spectral/spectral_dataset.cpp
)
target_include_directories(test_reassignment PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/media>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
)
target_link_libraries(test_reassignment PRIVATE SpectralCore)
set_target_properties(test_reassignment PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME reassignment COMMAND test_reassignment)
```

- [ ] **Step 3: Build and run tests**

Run: `cmake --build build --config Release && ctest --test-dir build/ -C Release -R reassignment --output-on-failure`
Expected: Tests pass

- [ ] **Step 4: Commit**

```bash
git add tests/phase12/test_reassignment.cpp CMakeLists.txt
git commit -m "test(reassignment): add numerical tests for time-frequency reassignment"
```

---

### Task 6: Add regression tests

**Files:**
- Create: `tests/phase12/test_reassignment_render.cpp`
- Modify: `CMakeLists.txt`

**Tests:**
- Render conventional spectrogram
- Render reassigned spectrogram
- Compare output dimensions
- Verify reassigned has fewer spread coefficients
- Verify both produce valid output

- [ ] **Step 1: Create test file**

Create `tests/phase12/test_reassignment_render.cpp`:

```cpp
// tests/phase12/test_reassignment_render.cpp
#include "spectrogram_renderer.h"
#include "spectrum_renderer.h"
#include "concrete_analyzer.h"
#include "spectral_dataset.h"
#include <cstdio>
#include <cmath>
#include <vector>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

static Spectral::SpectralDataset make_test_ds(bool reassigned) {
    using namespace Spectral;
    SpectralDataset ds;
    const int sr = 44100, fft = 1024, hop = 512, nf = 20;
    ds.mutable_frequency_axis() = FrequencyAxis(fft, sr);
    
    for (int i = 0; i < nf; ++i) {
        // Create signal with time-varying frequency
        std::vector<float> samples(fft);
        float freq = 500.0f + 1500.0f * static_cast<float>(i) / nf;
        for (int n = 0; n < fft; ++n) {
            float t = static_cast<float>(n) / sr;
            samples[n] = std::sin(2.0f * PI * freq * t);
        }
        
        ConcreteSpectralAnalyzer analyzer;
        analyzer.set_fft_size(fft);
        analyzer.set_reassignment(reassigned);
        
        SpectralFrame f;
        analyzer.analyze_frame(samples.data(), fft, f);
        f.frame_index = i;
        f.timestamp = double(i * hop) / sr;
        ds.add_frame(f);
    }
    
    ds.mutable_time_axis() = TimeAxis(nf, hop, sr);
    auto& am = ds.mutable_analysis_metadata();
    am.fft_size = fft; am.hop_size = hop; am.sample_rate = sr;
    return ds;
}

static void test_conventional_vs_reassigned() {
    std::fprintf(stderr, "[test_conventional_vs_reassigned]\n");
    
    auto ds_conv = make_test_ds(false);
    auto ds_reas = make_test_ds(true);
    
    // Both should have same dimensions
    CHECK(ds_conv.frame_count() == ds_reas.frame_count(), "same frame count");
    CHECK(ds_conv.num_frequency_bins() == ds_reas.num_frequency_bins(), "same freq bins");
    
    // Reassigned should have non-empty reassigned coordinates
    CHECK(!ds_reas.frame(0).reassigned_freqs.empty(), "reassigned has freq coords");
    CHECK(!ds_reas.frame(0).reassigned_times.empty(), "reassigned has time coords");
    
    // Conventional should have empty reassigned coordinates
    CHECK(ds_conv.frame(0).reassigned_freqs.empty(), "conventional has no freq coords");
    CHECK(ds_conv.frame(0).reassigned_times.empty(), "conventional has no time coords");
}

static void test_reassigned_spectrogram_render() {
    std::fprintf(stderr, "[test_reassigned_spectrogram_render]\n");
    
    auto ds = make_test_ds(true);
    
    Spectral::SpectrogramConfig cfg;
    cfg.width = 256; cfg.height = 128;
    cfg.freq_scale = Spectral::FrequencyScale::Linear;
    cfg.freq_min_hz = 20.0f; cfg.freq_max_hz = 20000.0f;
    
    Spectral::SpectrogramRenderer rend(cfg);
    Spectral::RGBAImage img;
    auto err = rend.render(ds, img);
    CHECK(err == Spectral::RenderError::Ok, "reassigned spectrogram renders");
    CHECK(img.width == 256 && img.height == 128, "reassigned spectrogram dims");
    
    int nonzero = 0;
    for (size_t p = 0; p < img.pixels.size(); p += 4) {
        if (img.pixels[p] > 0 || img.pixels[p+1] > 0 || img.pixels[p+2] > 0)
            nonzero++;
    }
    CHECK(nonzero > 100, "reassigned spectrogram has content");
}

int main() {
    test_conventional_vs_reassigned();
    test_reassigned_spectrogram_render();
    std::fprintf(stderr, "\n=== reassignment_render: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt**

Add to `CMakeLists.txt`:

```cmake
add_executable(test_reassignment_render
    tests/phase12/test_reassignment_render.cpp
    src/core/spectral/concrete_analyzer.cpp
    src/core/spectral/spectral_dataset.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/spectrum_renderer.cpp
    src/core/rendering/png_encoder.cpp
)
target_include_directories(test_reassignment_render PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/media>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
)
target_link_libraries(test_reassignment_render PRIVATE SpectralCore)
set_target_properties(test_reassignment_render PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME reassignment_render COMMAND test_reassignment_render)
```

- [ ] **Step 3: Build and run tests**

Run: `cmake --build build --config Release && ctest --test-dir build/ -C Release -R "reassignment" --output-on-failure`
Expected: All reassignment tests pass

- [ ] **Step 4: Commit**

```bash
git add tests/phase12/test_reassignment_render.cpp CMakeLists.txt
git commit -m "test(reassignment): add regression tests comparing conventional vs reassigned"
```

---

### Task 7: Write mathematical documentation

**Files:**
- Create: `docs/phase12-reassignment.md`

**Content:**
- Mathematical definition of reassignment
- Reference to Auger & Flandrin (1995)
- Implementation details
- Usage examples
- Comparison with conventional STFT

- [ ] **Step 1: Create documentation file**

Create `docs/phase12-reassignment.md`:

```markdown
# Phase 12 — Time-Frequency Reassignment

## Overview

Time-frequency reassignment improves the time-frequency resolution of the STFT by reassigning each coefficient to the center of gravity of the signal's energy in the time-frequency plane.

## Mathematical Method

Given the STFT X(t,k) at time frame t and frequency bin k:

### Instantaneous Frequency (Reassignment in Frequency)

ω̂(t,k) = Im{X* · ∂X/∂t} / (2π · |X|²)

Where:
- X* is the complex conjugate of the STFT
- ∂X/∂t is the time derivative of the STFT
- |X|² is the power spectrum

### Group Delay (Reassignment in Time)

τ̂(t,k) = Re{X* · ∂X/∂k} / (2π · |X|²)

Where:
- ∂X/∂k is the frequency derivative of the STFT

### Computing Derivatives

The derivatives are computed using the following properties:

1. **Time derivative:** ∂X/∂t = FFT{n · w[n] · x[n]}
   - Multiply the windowed signal by the sample index n before FFT

2. **Frequency derivative:** ∂X/∂k = k · X[k]
   - Multiply the STFT by the bin index k after FFT

## Reference

F. Auger and P. Flandrin, "Improving the readability of time-frequency and time-scale representations by the reassignment method," IEEE Trans. Signal Processing, vol. 43, no. 5, pp. 1068–1089, May 1995.

## Usage

Enable reassignment with the `--reassigned` flag:

```bash
spectragen --input audio.wav --output spectrogram.png --reassigned
```

## Comparison with Conventional STFT

| Aspect | Conventional STFT | Reassigned STFT |
|--------|-------------------|-----------------|
| Resolution | Limited by window size | Improved localization |
| Computational cost | O(N log N) | O(N log N) + O(N) |
| Interpretability | Standard time-frequency grid | Corrected coordinates |
| Use case | General purpose | High-resolution analysis |

## When to Use Reassignment

- **Impulse detection:** Reassignment centers impulses at the correct time
- **Tone localization:** Pure tones are placed at the exact frequency
- **Chirp tracking:** Time-varying frequencies follow the instantaneous value
- **Transient analysis:** Short-duration events are better localized

## When NOT to Use Reassignment

- **Noise signals:** Reassignment can scatter noise energy
- **Low SNR:** Reassignment requires accurate phase estimation
- **Standard analysis:** When conventional STFT resolution is sufficient
```

- [ ] **Step 2: Commit**

```bash
git add docs/phase12-reassignment.md
git commit -m "docs(phase12): add mathematical documentation for time-frequency reassignment"
```

---

### Task 8: Final verification

**Files:**
- None (verification only)

- [ ] **Step 1: Build everything**

Run: `cmake --build build --config Release`
Expected: Build succeeds with no errors

- [ ] **Step 2: Run all tests**

Run: `ctest --test-dir build/ -C Release --output-on-failure`
Expected: All 13 tests pass (11 existing + 2 new reassignment tests)

- [ ] **Step 3: Verify --reassigned flag works**

Run: `build/Release/spectragen.exe --help`
Expected: `--reassigned` appears in help text

- [ ] **Step 4: Final commit**

```bash
git add -A
git commit -m "feat(phase12): complete time-frequency reassignment implementation"
```
