# Phase 11 — Advanced Spectral Representations

> **For agentic workers:** Use `superpowers:executing-plans` to implement this plan task-by-task.

**Goal:** Add 5 perceptual frequency scales (Mel, Bark, ERB, CQT, plus refactored Log) to spectrogram and spectrum renderers, with correct coordinate mapping and full test coverage.

**Architecture:** Pure-math `frequency_scale.h` header with Hz↔scale conversions and a unified `hz_to_unit()` normalizer. Extend `FrequencyScale` enum. Refactor existing renderers to use shared mapping. CLI `--freq-scale` flag.

**Tech Stack:** C++20, MSVC 19.51, CMake, no external deps.

**Spec:** `docs/superpowers/plans/2026-09-02-phase11-advanced-spectral-representations.md` (this file)

## Global Constraints

- MSVC 19.51, C++20, Windows
- No external deps (no libsndfile, no kfr)
- All DSP/FFT self-contained in `src/core/dsp/`
- Existing tests must continue passing (7 base + 3 Phase 10 = 10 suites)
- MSVC LSP errors are spurious — trust `cmake --build` output

## File Structure

| File | Action | Purpose |
|------|--------|---------|
| `src/core/dsp/frequency_scale.h` | Create | Pure math: Hz↔Mel/Bark/ERB/CQT + `hz_to_unit()` |
| `src/core/rendering/spectrogram_renderer.h` | Modify | Extend `FrequencyScale` enum + CQT config fields |
| `src/core/rendering/spectrogram_renderer.cpp` | Modify | Refactor `FreqMapper` to use `hz_to_unit()` |
| `src/core/rendering/spectrum_renderer.h` | Modify | Extend `SpectrumConfig` (inherits new enum) |
| `src/core/rendering/spectrum_renderer.cpp` | Modify | Refactor `freq_to_x()` to use `hz_to_unit()` |
| `src/cli/main.cpp` | Modify | `--freq-scale` flag parsing |
| `tests/phase11/test_frequency_scale.cpp` | Create | Unit tests: conversions, roundtrips, known values |
| `tests/phase11/test_freqscale_render.cpp` | Create | Rendering integration: each scale produces valid output |
| `CMakeLists.txt` | Modify | New test targets |

---

### Task 1: frequency_scale.h — Pure Math Module

**Files:**
- Create: `src/core/dsp/frequency_scale.h`

**Interfaces:**
- Produces: all conversion functions used by Tasks 3-5

- [ ] **Step 1: Create the header with all conversion functions**

```cpp
#pragma once

// Phase 11 — Perceptual frequency scale conversions.
// Pure functions, no state, no deps beyond <cmath>.

#include <cmath>
#include <algorithm>

namespace Spectral {

// ---- Mel scale (Stevens, Volkmann, Newman 1937) ----
// 0 mel = DC, 1000 mel ≈ 1 kHz
inline float hz_to_mel(float hz) {
    if (hz <= 0.0f) return 0.0f;
    return 2595.0f * std::log10(1.0f + hz / 700.0f);
}
inline float mel_to_hz(float mel) {
    if (mel <= 0.0f) return 0.0f;
    return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
}

// ---- Bark scale (Zwicker & Terhardt 1980) ----
// 0 Bark = DC, 24 Bark ≈ 15.5 kHz
inline float hz_to_bark(float hz) {
    if (hz <= 0.0f) return 0.0f;
    return 13.0f * std::atan(0.00076f * hz)
         + 3.5f * std::atan(hz * hz / (7500.0f * 7500.0f));
}
inline float bark_to_hz(float bark) {
    if (bark <= 0.0f) return 0.0f;
    // Inverse via bisection (monotonic, fast convergence)
    float lo = 0.0f, hi = 24000.0f;
    for (int i = 0; i < 24; ++i) {  // 24 iterations ≈ 1e-7 precision
        float mid = (lo + hi) * 0.5f;
        if (hz_to_bark(mid) < bark) lo = mid;
        else hi = mid;
    }
    return (lo + hi) * 0.5f;
}

// ---- ERB scale (Glasberg & Moore 1990) ----
// Equivalent Rectangular Bandwidth rate
inline float hz_to_erb(float hz) {
    if (hz <= 0.0f) return 0.0f;
    return 24.7f * (4.37f * hz / 1000.0f + 1.0f);
}
inline float erb_to_hz(float erb) {
    if (erb <= 0.0f) return 0.0f;
    return (erb / 24.7f - 1.0f) * 1000.0f / 4.37f;
}

// ---- CQT bin index (Schörkhuber 2010) ----
// Maps Hz to a fractional CQT bin index given center frequency and Q.
// bin(hz) = Q * log2(hz / f_center)
inline float hz_to_cqt_bin(float hz, float f_center, float Q) {
    if (hz <= 0.0f || f_center <= 0.0f || Q <= 0.0f) return 0.0f;
    return Q * std::log2(hz / f_center);
}

// ---- Unified normalizer: Hz → [0, 1] ----
// For Mel/Bark/ERB: linear interpolation between scale(fmin) and scale(fmax).
// For Logarithmic: log10 interpolation.
// For Linear: direct linear.
// For CQT: maps bin index to [0,1] over the configured range.
inline float hz_to_unit(float hz, int scale_enum,
                        float fmin, float fmax,
                        float cqt_center = 0.0f, float cqt_q = 0.0f) {
    if (fmax <= fmin || hz < 0.0f) return 0.0f;
    float t = 0.0f;
    switch (scale_enum) {
        case 0: // Linear
            t = (hz - fmin) / (fmax - fmin);
            break;
        case 1: { // Logarithmic
            const float lo = std::log10(std::max(fmin, 1.0f));
            const float hi = std::log10(std::max(fmax, 1.0f));
            const float lf = std::log10(std::max(hz, 1.0f));
            t = (hi > lo) ? (lf - lo) / (hi - lo) : 0.0f;
            break;
        }
        case 2: { // Mel
            float mmin = hz_to_mel(fmin);
            float mmax = hz_to_mel(fmax);
            t = (mmax > mmin) ? (hz_to_mel(hz) - mmin) / (mmax - mmin) : 0.0f;
            break;
        }
        case 3: { // Bark
            float bmin = hz_to_bark(fmin);
            float bmax = hz_to_bark(fmax);
            t = (bmax > bmin) ? (hz_to_bark(hz) - bmin) / (bmax - bmin) : 0.0f;
            break;
        }
        case 4: { // ERB
            float emin = hz_to_erb(fmin);
            float emax = hz_to_erb(fmax);
            t = (emax > emin) ? (hz_to_erb(hz) - emin) / (emax - emin) : 0.0f;
            break;
        }
        case 5: { // CQT
            if (cqt_center <= 0.0f || cqt_q <= 0.0f) {
                // Fallback to log if CQT params missing
                const float lo = std::log10(std::max(fmin, 1.0f));
                const float hi = std::log10(std::max(fmax, 1.0f));
                const float lf = std::log10(std::max(hz, 1.0f));
                t = (hi > lo) ? (lf - lo) / (hi - lo) : 0.0f;
            } else {
                float bmin = hz_to_cqt_bin(fmin, cqt_center, cqt_q);
                float bmax = hz_to_cqt_bin(fmax, cqt_center, cqt_q);
                float bcur = hz_to_cqt_bin(hz, cqt_center, cqt_q);
                t = (bmax > bmin) ? (bcur - bmin) / (bmax - bmin) : 0.0f;
            }
            break;
        }
        default:
            t = 0.0f;
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

} // namespace Spectral
```

- [ ] **Step 2: Verify header compiles**

Run: `cmake --build build/ --config Release --target SpectralCore` (or just ensure no compile errors when included)

- [ ] **Step 3: Commit**

```bash
git add src/core/dsp/frequency_scale.h
git commit -m "feat(phase11): add frequency_scale.h with Mel/Bark/ERB/CQT conversions"
```

---

### Task 2: Unit Tests for frequency_scale.h

**Files:**
- Create: `tests/phase11/test_frequency_scale.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `frequency_scale.h` (Task 1)

- [ ] **Step 1: Write the test file**

```cpp
// tests/phase11/test_frequency_scale.cpp
#include "frequency_scale.h"
#include <cstdio>
#include <cmath>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

static void test_mel_conversion() {
    std::fprintf(stderr, "[test_mel_conversion]\n");
    // 0 Hz = 0 mel
    CHECK(std::abs(Spectral::hz_to_mel(0.0f)) < 0.01f, "0 Hz = 0 mel");
    // 1000 Hz = 1000 mel (by definition)
    CHECK(std::abs(Spectral::hz_to_mel(1000.0f) - 1000.0f) < 1.0f, "1000 Hz ~= 1000 mel");
    // Roundtrip
    for (float hz : {100.0f, 440.0f, 1000.0f, 4000.0f, 8000.0f, 16000.0f}) {
        float mel = Spectral::hz_to_mel(hz);
        float back = Spectral::mel_to_hz(mel);
        CHECK(std::abs(back - hz) / hz < 0.01f, "mel roundtrip");
    }
}

static void test_bark_conversion() {
    std::fprintf(stderr, "[test_bark_conversion]\n");
    CHECK(std::abs(Spectral::hz_to_bark(0.0f)) < 0.01f, "0 Hz = 0 bark");
    // 1000 Hz ≈ 8.7 bark (published table value ~8.5-9.0)
    float b1k = Spectral::hz_to_bark(1000.0f);
    CHECK(b1k > 8.0f && b1k < 9.5f, "1000 Hz ~= 8.7 bark");
    // Roundtrip
    for (float hz : {100.0f, 440.0f, 1000.0f, 4000.0f, 8000.0f}) {
        float bark = Spectral::hz_to_bark(hz);
        float back = Spectral::bark_to_hz(bark);
        CHECK(std::abs(back - hz) / hz < 0.02f, "bark roundtrip");
    }
}

static void test_erb_conversion() {
    std::fprintf(stderr, "[test_erb_conversion]\n");
    CHECK(std::abs(Spectral::hz_to_erb(0.0f)) < 0.01f, "0 Hz = 0 ERB-rate");
    // 1000 Hz ≈ 24.7*(4.37+1) ≈ 132.6 ERB-rate
    float e1k = Spectral::hz_to_erb(1000.0f);
    CHECK(e1k > 120.0f && e1k < 140.0f, "1000 Hz ~= 132 ERB-rate");
    // Roundtrip
    for (float hz : {50.0f, 200.0f, 1000.0f, 6000.0f, 12000.0f}) {
        float erb = Spectral::hz_to_erb(hz);
        float back = Spectral::erb_to_hz(erb);
        CHECK(std::abs(back - hz) / hz < 0.01f, "erb roundtrip");
    }
}

static void test_cqt_bin() {
    std::fprintf(stderr, "[test_cqt_bin]\n");
    // bin at center freq = 0
    CHECK(std::abs(Spectral::hz_to_cqt_bin(440.0f, 440.0f, 12.0f)) < 0.01f,
          "center bin = 0");
    // bin above center > 0
    CHECK(Spectral::hz_to_cqt_bin(880.0f, 440.0f, 12.0f) > 0.0f, "octave up > 0");
    // 880 Hz at Q=12: 12 * log2(2) = 12
    float b = Spectral::hz_to_cqt_bin(880.0f, 440.0f, 12.0f);
    CHECK(std::abs(b - 12.0f) < 0.1f, "octave = 12 bins at Q=12");
}

static void test_hz_to_unit_scales() {
    std::fprintf(stderr, "[test_hz_to_unit_scales]\n");
    float fmin = 20.0f, fmax = 20000.0f;
    // fmin maps to 0, fmax maps to 1 for all scales
    CHECK(std::abs(Spectral::hz_to_unit(fmin, 0, fmin, fmax) - 0.0f) < 0.01f, "linear: fmin=0");
    CHECK(std::abs(Spectral::hz_to_unit(fmax, 0, fmin, fmax) - 1.0f) < 0.01f, "linear: fmax=1");
    CHECK(std::abs(Spectral::hz_to_unit(fmin, 1, fmin, fmax) - 0.0f) < 0.01f, "log: fmin=0");
    CHECK(std::abs(Spectral::hz_to_unit(fmax, 1, fmin, fmax) - 1.0f) < 0.01f, "log: fmax=1");
    CHECK(std::abs(Spectral::hz_to_unit(fmin, 2, fmin, fmax) - 0.0f) < 0.01f, "mel: fmin=0");
    CHECK(std::abs(Spectral::hz_to_unit(fmax, 2, fmin, fmax) - 1.0f) < 0.01f, "mel: fmax=1");
    CHECK(std::abs(Spectral::hz_to_unit(fmin, 3, fmin, fmax) - 0.0f) < 0.01f, "bark: fmin=0");
    CHECK(std::abs(Spectral::hz_to_unit(fmax, 3, fmin, fmax) - 1.0f) < 0.01f, "bark: fmax=1");
    CHECK(std::abs(Spectral::hz_to_unit(fmin, 4, fmin, fmax) - 0.0f) < 0.01f, "erb: fmin=0");
    CHECK(std::abs(Spectral::hz_to_unit(fmax, 4, fmin, fmax) - 1.0f) < 0.01f, "erb: fmax=1");
    // Midpoints differ by scale — just verify monotonicity
    float mid_lin = Spectral::hz_to_unit(1000.0f, 0, fmin, fmax);
    float mid_log = Spectral::hz_to_unit(1000.0f, 1, fmin, fmax);
    float mid_mel = Spectral::hz_to_unit(1000.0f, 2, fmin, fmax);
    CHECK(mid_lin > 0.0f && mid_lin < 1.0f, "linear mid in (0,1)");
    CHECK(mid_log > 0.0f && mid_log < 1.0f, "log mid in (0,1)");
    CHECK(mid_mel > 0.0f && mid_mel < 1.0f, "mel mid in (0,1)");
}

static void test_monotonicity() {
    std::fprintf(stderr, "[test_monotonicity]\n");
    float fmin = 20.0f, fmax = 20000.0f;
    // For each scale, verify hz_to_unit is monotonically increasing
    float prev = -1.0f;
    for (float hz = 20.0f; hz <= 20000.0f; hz *= 1.5f) {
        for (int scale = 0; scale <= 4; ++scale) {
            float u = Spectral::hz_to_unit(hz, scale, fmin, fmax);
            if (prev >= 0.0f) CHECK(u >= prev - 0.001f, "monotonic");
            prev = u;
        }
        prev = -1.0f;
    }
}

int main() {
    test_mel_conversion();
    test_bark_conversion();
    test_erb_conversion();
    test_cqt_bin();
    test_hz_to_unit_scales();
    test_monotonicity();
    std::fprintf(stderr, "\n=== frequency_scale: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
```

- [ ] **Step 2: Add CMake target**

Add to `CMakeLists.txt` after the Phase 10 section:

```cmake
# --- --- Phase 11: Frequency scales ---
add_executable(test_frequency_scale
    tests/phase11/test_frequency_scale.cpp
)
target_include_directories(test_frequency_scale PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
set_target_properties(test_frequency_scale PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME frequency_scale COMMAND test_frequency_scale)
```

- [ ] **Step 3: Build and run test**

```bash
cmake --build build/ --config Release --target test_frequency_scale
ctest --test-dir build/ -C Release -R frequency_scale --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/phase11/test_frequency_scale.cpp CMakeLists.txt
git commit -m "feat(phase11): add unit tests for frequency scale conversions"
```

---

### Task 3: Extend FrequencyScale Enum + Refactor Renderers

**Files:**
- Modify: `src/core/rendering/spectrogram_renderer.h:38-41` (enum + config)
- Modify: `src/core/rendering/spectrogram_renderer.cpp:153-181,251-309` (FreqMapper + render)
- Modify: `src/core/rendering/spectrum_renderer.cpp:403-418` (freq_to_x)

**Interfaces:**
- Consumes: `frequency_scale.h` (Task 1)
- Produces: extended enum used by Tasks 4, 5

- [ ] **Step 1: Extend the FrequencyScale enum in spectrogram_renderer.h**

Replace:
```cpp
enum class FrequencyScale {
    Linear = 0,
    Logarithmic = 1,
};
```

With:
```cpp
enum class FrequencyScale {
    Linear = 0,
    Logarithmic = 1,
    Mel = 2,
    Bark = 3,
    Erb = 4,
    CQT = 5,
};
```

- [ ] **Step 2: Add CQT config fields to SpectrogramConfig**

After `float freq_max_hz = 0.0f;` add:
```cpp
    float cqt_center_hz = 440.0f;  // CQT center frequency (Hz)
    float cqt_q = 12.0f;           // CQT quality factor (bins per octave)
```

- [ ] **Step 3: Add same fields to SpectrumConfig in spectrum_renderer.h**

After `float freq_max_hz = 0.0f;` in `SpectrumConfig` add:
```cpp
    float cqt_center_hz = 440.0f;
    float cqt_q = 12.0f;
```

- [ ] **Step 4: Refactor FreqMapper in spectrogram_renderer.cpp to use hz_to_unit()**

Add `#include "frequency_scale.h"` at top. Replace the `FreqMapper` struct with:

```cpp
struct FreqMapper {
    int scale_enum = 1;  // FrequencyScale value
    float fmin = 20.0f;
    float fmax = 20000.0f;
    int height = 0;
    float cqt_center = 0.0f;
    float cqt_q = 0.0f;

    float row_for(float freq) const {
        if (height <= 0) return 0.0f;
        float t = hz_to_unit(freq, scale_enum, fmin, fmax, cqt_center, cqt_q);
        // row 0 = top = highest freq. Invert: row = (1-t) * (H-1)
        return (1.0f - t) * static_cast<float>(height - 1);
    }
};
```

Update the FreqMapper construction in `render()` (around line 251):
```cpp
    const FreqMapper fm{static_cast<int>(cfg_.freq_scale),
                        fmin, fmax, H,
                        cfg_.cqt_center_hz, cfg_.cqt_q};
```

- [ ] **Step 5: Refactor the per-pixel frequency mapping in render()**

Replace the inline frequency mapping block (around lines 300-309):
```cpp
                const double low_frac = 1.0 - static_cast<double>(y) / (H - 1);
                float freq;
                if (cfg_.freq_scale == FrequencyScale::Logarithmic) {
                    const double lo_lf = std::log10(static_cast<double>(fmin));
                    const double hi_lf = std::log10(static_cast<double>(fmax));
                    const double lf = lo_lf + low_frac * (hi_lf - lo_lf);
                    freq = static_cast<float>(std::pow(10.0, lf));
                } else {
                    freq = fmin + static_cast<float>(low_frac) * (fmax - fmin);
                }
```

With (using inverse mapping):
```cpp
                // Invert unit [0,1] → Hz using the active scale.
                // low_frac = 1 at top (y=0), 0 at bottom (y=H-1).
                const double low_frac = 1.0 - static_cast<double>(y) / (H - 1);
                float freq = unit_to_hz(static_cast<float>(low_frac),
                                        static_cast<int>(cfg_.freq_scale),
                                        fmin, fmax,
                                        cfg_.cqt_center_hz, cfg_.cqt_q);
```

- [ ] **Step 6: Add unit_to_hz() to frequency_scale.h**

Add after `hz_to_unit()`:
```cpp
// Inverse of hz_to_unit: [0,1] → Hz for each scale.
// Uses bisection (monotonic guarantee).
inline float unit_to_hz(float u, int scale_enum,
                        float fmin, float fmax,
                        float cqt_center = 0.0f, float cqt_q = 0.0f) {
    if (u <= 0.0f) return fmin;
    if (u >= 1.0f) return fmax;
    // Bisection: 40 iterations ≈ 1e-12 precision for [20, 20000]
    float lo = fmin, hi = fmax;
    for (int i = 0; i < 40; ++i) {
        float mid = (lo + hi) * 0.5f;
        float umid = hz_to_unit(mid, scale_enum, fmin, fmax, cqt_center, cqt_q);
        if (umid < u) lo = mid;
        else hi = mid;
    }
    return (lo + hi) * 0.5f;
}
```

- [ ] **Step 7: Refactor SpectrumRenderer::freq_to_x() to use hz_to_unit()**

In `spectrum_renderer.cpp`, add `#include "frequency_scale.h"`. Replace `freq_to_x()` body:

```cpp
int SpectrumRenderer::freq_to_x(float freq_hz, float fmin, float fmax,
                                 int width, FrequencyScale scale) {
    if (fmax <= fmin || width <= 0) return 0;
    float t = hz_to_unit(freq_hz, static_cast<int>(scale), fmin, fmax);
    return static_cast<int>(std::lround(t * static_cast<float>(width - 1)));
}
```

- [ ] **Step 8: Build all and verify existing 10 tests still pass**

```bash
cmake --build build/ --config Release
ctest --test-dir build/ -C Release --output-on-failure
```

Expected: 11 tests pass (10 existing + frequency_scale).

- [ ] **Step 9: Commit**

```bash
git add src/core/dsp/frequency_scale.h src/core/rendering/spectrogram_renderer.h \
        src/core/rendering/spectrogram_renderer.cpp src/core/rendering/spectrum_renderer.h \
        src/core/rendering/spectrum_renderer.cpp
git commit -m "feat(phase11): extend FrequencyScale enum, refactor renderers to use shared mapping"
```

---

### Task 4: CLI --freq-scale Flag

**Files:**
- Modify: `src/cli/main.cpp`

**Interfaces:**
- Consumes: `FrequencyScale` enum (Task 3)

- [ ] **Step 1: Add freq_scale field to CliConfig**

In the `CliConfig` struct, add:
```cpp
    std::string freq_scale = "log";  // linear|log|mel|bark|erb|cqt
    float cqt_center = 440.0f;
    float cqt_q = 12.0f;
```

- [ ] **Step 2: Add --freq-scale flag parsing**

In `parse_args()`, add after the `--max-frequency` block:
```cpp
        if (arg == "--freq-scale") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.freq_scale = argv[++i];
            if (cfg.freq_scale != "linear" && cfg.freq_scale != "log" &&
                cfg.freq_scale != "mel" && cfg.freq_scale != "bark" &&
                cfg.freq_scale != "erb" && cfg.freq_scale != "cqt") {
                std::cerr << "Error: --freq-scale must be linear, log, mel, bark, erb, or cqt\n";
                return 1;
            }
            continue;
        }
        if (arg == "--cqt-center") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.cqt_center = static_cast<float>(std::atof(argv[++i]));
            if (cfg.cqt_center <= 0.0f) { std::cerr << "Error: --cqt-center must be > 0\n"; return 1; }
            continue;
        }
        if (arg == "--cqt-q") {
            if (i + 1 >= argc) { std::cerr << "Error: " << arg << " requires a value\n"; return 1; }
            cfg.cqt_q = static_cast<float>(std::atof(argv[++i]));
            if (cfg.cqt_q <= 0.0f) { std::cerr << "Error: --cqt-q must be > 0\n"; return 1; }
            continue;
        }
```

- [ ] **Step 3: Add helper to map string → FrequencyScale**

Add a static helper near the top of `main.cpp`:
```cpp
static Spectral::FrequencyScale parse_freq_scale(const std::string& s) {
    if (s == "linear") return Spectral::FrequencyScale::Linear;
    if (s == "mel")    return Spectral::FrequencyScale::Mel;
    if (s == "bark")   return Spectral::FrequencyScale::Bark;
    if (s == "erb")    return Spectral::FrequencyScale::Erb;
    if (s == "cqt")    return Spectral::FrequencyScale::CQT;
    return Spectral::FrequencyScale::Logarithmic;
}
```

- [ ] **Step 4: Use it in the render dispatch**

In the spectrogram/spectrum/video render config sections, replace:
```cpp
sc.freq_scale = Spectral::FrequencyScale::Logarithmic;
```

With:
```cpp
sc.freq_scale = parse_freq_scale(cfg.freq_scale);
sc.cqt_center_hz = cfg.cqt_center;
sc.cqt_q = cfg.cqt_q;
```

Do the same for `SpectrumConfig` and `VideoRendererConfig`.

- [ ] **Step 5: Add to help text**

In `print_usage()`, add after `--max-frequency`:
```cpp
        "  --freq-scale <scale>        linear | log | mel | bark | erb | cqt (default: log)\n"
        "  --cqt-center <hz>           CQT center frequency (default: 440)\n"
        "  --cqt-q <factor>            CQT quality factor / bins per octave (default: 12)\n"
```

- [ ] **Step 6: Build and verify**

```bash
cmake --build build/ --config Release
ctest --test-dir build/ -C Release --output-on-failure
```

Expected: 11 tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/cli/main.cpp
git commit -m "feat(phase11): add --freq-scale flag for mel/bark/erb/cqt rendering"
```

---

### Task 5: Rendering Integration Tests

**Files:**
- Create: `tests/phase11/test_freqscale_render.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SpectrogramRenderer`, `SpectrumRenderer`, `FrequencyScale` (Tasks 3-4)

- [ ] **Step 1: Write the integration test file**

```cpp
// tests/phase11/test_freqscale_render.cpp
#include "spectrogram_renderer.h"
#include "spectrum_renderer.h"
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

// Synthetic dataset: 440 Hz + 2000 Hz sine, 44100 Hz, 1 second, 1024 FFT
static Spectral::SpectralDataset make_test_ds() {
    using namespace Spectral;
    SpectralDataset ds;
    const int sr = 44100, fft = 1024, hop = 512, nf = 80;
    ds.mutable_frequency_axis() = FrequencyAxis(fft, sr);
    for (int i = 0; i < nf; ++i) {
        SpectralFrame f;
        f.frame_index = i;
        f.n_fft = fft;
        f.window_factor = 0.5f;
        f.timestamp = double(i * hop) / sr;
        f.magnitudes.resize(fft / 2 + 1, 0.0f);
        f.phases.resize(fft / 2 + 1, 0.0f);
        f.power.resize(fft / 2 + 1, 0.0f);
        // Energy at bin ~19 (440 Hz) and bin ~93 (2000 Hz)
        for (int k = 0; k < fft / 2 + 1; ++k) {
            float d1 = std::abs(k - 19.0f);
            float d2 = std::abs(k - 93.0f);
            float mag = std::exp(-d1*d1/8.0f)*0.3f + std::exp(-d2*d2/8.0f)*0.2f;
            f.magnitudes[k] = mag;
            f.power[k] = mag * mag;
        }
        f.rms = 0.15f;
        f.peak_magnitude = 0.5f;
        f.spectral_centroid = 1000.0f;
        ds.add_frame(f);
    }
    ds.mutable_time_axis() = TimeAxis(nf, hop, sr);
    auto& am = ds.mutable_analysis_metadata();
    am.fft_size = fft; am.hop_size = hop; am.sample_rate = sr;
    return ds;
}

static void test_spectrogram_all_scales() {
    std::fprintf(stderr, "[test_spectrogram_all_scales]\n");
    auto ds = make_test_ds();
    const char* names[] = {"linear", "log", "mel", "bark", "erb", "cqt"};
    Spectral::FrequencyScale scales[] = {
        Spectral::FrequencyScale::Linear,
        Spectral::FrequencyScale::Logarithmic,
        Spectral::FrequencyScale::Mel,
        Spectral::FrequencyScale::Bark,
        Spectral::FrequencyScale::Erb,
        Spectral::FrequencyScale::CQT,
    };
    for (int i = 0; i < 6; ++i) {
        Spectral::SpectrogramConfig cfg;
        cfg.width = 256; cfg.height = 128;
        cfg.freq_scale = scales[i];
        cfg.freq_min_hz = 20.0f; cfg.freq_max_hz = 20000.0f;
        if (scales[i] == Spectral::FrequencyScale::CQT) {
            cfg.cqt_center_hz = 440.0f;
            cfg.cqt_q = 12.0f;
        }
        Spectral::SpectrogramRenderer rend(cfg);
        Spectral::RGBAImage img;
        auto err = rend.render(ds, img);
        CHECK(err == Spectral::RenderError::Ok,
              (std::string("spectrogram ") + names[i] + " renders").c_str());
        CHECK(img.width == 256 && img.height == 128,
              (std::string("spectrogram ") + names[i] + " dims").c_str());
        // Verify non-trivial content (not all black)
        int nonzero = 0;
        for (size_t p = 0; p < img.pixels.size(); p += 4) {
            if (img.pixels[p] > 0 || img.pixels[p+1] > 0 || img.pixels[p+2] > 0)
                nonzero++;
        }
        CHECK(nonzero > 100,
              (std::string("spectrogram ") + names[i] + " has content").c_str());
    }
}

static void test_spectrum_all_scales() {
    std::fprintf(stderr, "[test_spectrum_all_scales]\n");
    auto ds = make_test_ds();
    const char* names[] = {"linear", "log", "mel", "bark", "erb", "cqt"};
    Spectral::FrequencyScale scales[] = {
        Spectral::FrequencyScale::Linear,
        Spectral::FrequencyScale::Logarithmic,
        Spectral::FrequencyScale::Mel,
        Spectral::FrequencyScale::Bark,
        Spectral::FrequencyScale::Erb,
        Spectral::FrequencyScale::CQT,
    };
    for (int i = 0; i < 6; ++i) {
        Spectral::SpectrumConfig cfg;
        cfg.width = 256; cfg.height = 128;
        cfg.freq_scale = scales[i];
        cfg.freq_min_hz = 20.0f; cfg.freq_max_hz = 20000.0f;
        if (scales[i] == Spectral::FrequencyScale::CQT) {
            cfg.cqt_center_hz = 440.0f;
            cfg.cqt_q = 12.0f;
        }
        Spectral::SpectrumRenderer rend(cfg);
        Spectral::RGBAImage img;
        auto err = rend.render(ds, img);
        CHECK(err == Spectral::SpectrumError::Ok,
              (std::string("spectrum ") + names[i] + " renders").c_str());
    }
}

int main() {
    test_spectrogram_all_scales();
    test_spectrum_all_scales();
    std::fprintf(stderr, "\n=== freqscale_render: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
```

- [ ] **Step 2: Add CMake target**

```cmake
add_executable(test_freqscale_render
    tests/phase11/test_freqscale_render.cpp
    src/core/spectral/spectral_dataset.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/spectrum_renderer.cpp
    src/core/rendering/png_encoder.cpp
)
target_include_directories(test_freqscale_render PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
)
target_link_libraries(test_freqscale_render PRIVATE SpectralCore)
set_target_properties(test_freqscale_render PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME freqscale_render COMMAND test_freqscale_render)
```

- [ ] **Step 3: Build and run all tests**

```bash
cmake --build build/ --config Release
ctest --test-dir build/ -C Release --output-on-failure
```

Expected: 12 tests pass (10 base + frequency_scale + freqscale_render).

- [ ] **Step 4: Commit**

```bash
git add tests/phase11/test_freqscale_render.cpp CMakeLists.txt
git commit -m "test(phase11): rendering integration tests for all frequency scales"
```

---

### Task 6: Final Verification

- [ ] **Step 1: Full rebuild + full test suite**

```bash
cmake --build build/ --config Release --clean-first
ctest --test-dir build/ -C Release --output-on-failure
```

Expected: 12/12 pass.

- [ ] **Step 2: Quick smoke test with real audio (if available)**

```bash
build\Release\spectragen.exe <any-audio-file> -o test_mel.png --freq-scale mel
build\Release\spectragen.exe <any-audio-file> -o test_bark.png --freq-scale bark
build\Release\spectragen.exe <any-audio-file> -o test_erb.png --freq-scale erb
build\Release\spectragen.exe <any-audio-file> -o test_cqt.png --freq-scale cqt --cqt-center 440 --cqt-q 12
```

Verify each produces a valid PNG file.

- [ ] **Step 3: Final commit (if any fixups needed)**
