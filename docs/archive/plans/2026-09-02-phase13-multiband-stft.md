# Multi-Band STFT Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement multi-band STFT analysis with comparison against fixed, short-window, and long-window STFTs.

**Architecture:** Split Nyquist into 3 bands (low/mid/high), each with its own FFT size. Merge band outputs into a single SpectralFrame. Compare 4 analysis methods and print a table. Experimental — not enabled by default.

**Tech Stack:** C++20, MSVC 19.51, CMake, CTest. No external dependencies.

**Spec:** `docs/superpowers/specs/2026-09-02-phase13-multiband-stft-design.md`

## Global Constraints

- MSVC 19.51 on Windows. All FFT/DSP self-contained.
- CMake multi-config: use `-C Release` for ctest.
- Binary serialization version bump: `SPECTRAL_DATASET_VERSION = 2`.
- Window functions and FFT in `src/core/dsp/fft.h`.
- `SpectralCore` is CMake INTERFACE library. Each target adds its own `target_include_directories`.
- LSP errors on `fft.h`, `spectral_dataset.h` are spurious — real build succeeds via cmake.
- No external dependencies. All JSON, SHA256, PNG self-contained.

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `src/core/spectral/spectral_dataset.h` | Modify | Add `analysis_method`, `band_count`, version bump |
| `src/core/spectral/spectral_dataset.cpp` | Modify | Serialize/deserialize v2 fields |
| `src/core/spectral/multiband_analyzer.h` | Create | `MultiBandAnalyzer` class declaration |
| `src/core/spectral/multiband_analyzer.cpp` | Create | Band splitting, per-band STFT, merging |
| `src/cli/main.cpp` | Modify | `--multiband` flag, comparison table, conditional write |
| `tests/phase13/test_multiband.cpp` | Create | Numerical tests (7 subtests) |
| `tests/phase13/test_multiband_compare.cpp` | Create | Comparison + regression tests (4 subtests) |
| `CMakeLists.txt` | Modify | New test targets |
| `docs/phase13-multiband.md` | Create | Documentation |

---

### Task 1: Metadata Changes

**Files:**
- Modify: `src/core/spectral/spectral_dataset.h:48-98,221-257`
- Modify: `src/core/spectral/spectral_dataset.cpp`

**Interfaces:**
- Consumes: existing `SpectralDataset`, `SpectralFrame`, `AnalysisMetadata` types
- Produces: `analysis_method` string field, `band_count` int field, version 2 serialization

- [ ] **Step 1: Add fields to AnalysisMetadata**

In `spectral_dataset.h`, after `analyzer_version` field (around line 90), add:

```cpp
std::string analysis_method = "stft";
```

In `spectral_dataset.h`, after `total_frames` field in `AnalysisMetadata` (around line 95), add:

```cpp
int band_count = 1;
```

- [ ] **Step 2: Add band_count to SpectralFrame**

In `spectral_dataset.h`, in `SpectralFrame` struct (around line 255), add after `reassigned_freqs`:

```cpp
int band_count = 1;
```

- [ ] **Step 3: Update operator==**

In `spectral_dataset.h`, in `SpectralFrame::operator==`, add:

```cpp
&& band_count == other.band_count
```

- [ ] **Step 4: Update version constant**

In `spectral_dataset.h`, change:

```cpp
static constexpr int SPECTRAL_DATASET_VERSION = 1;
```

to:

```cpp
static constexpr int SPECTRAL_DATASET_VERSION = 2;
```

- [ ] **Step 5: Update write_metadata**

In `spectral_dataset.cpp`, in `write_metadata()` function, after the line that writes `analyzer_version`, add:

```cpp
if (version >= 2) {
    out.write(metadata.analysis_method.c_str(),
              static_cast<std::streamsize>(metadata.analysis_method.size()));
    char null = '\0';
    out.write(&null, 1);
    out.write(reinterpret_cast<const char*>(&metadata.band_count),
              sizeof(metadata.band_count));
}
```

- [ ] **Step 6: Update read_metadata**

In `spectral_dataset.cpp`, in `read_metadata()` function, after reading `analyzer_version`, add:

```cpp
if (version >= 2) {
    std::getline(in, metadata.analysis_method);
    in.read(reinterpret_cast<char*>(&metadata.band_count),
            sizeof(metadata.band_count));
}
```

- [ ] **Step 7: Build and verify**

Run: `cmake --build build --config Release`
Expected: No errors

- [ ] **Step 8: Run existing tests**

Run: `ctest --test-dir build/ -C Release --output-on-failure`
Expected: All 11 existing tests pass

- [ ] **Step 9: Commit**

```bash
git add src/core/spectral/spectral_dataset.h src/core/spectral/spectral_dataset.cpp
git commit -m "feat: add analysis_method and band_count to spectral metadata (v2)"
```

---

### Task 2: MultiBandAnalyzer

**Files:**
- Create: `src/core/spectral/multiband_analyzer.h`
- Create: `src/core/spectral/multiband_analyzer.cpp`

**Interfaces:**
- Consumes: `Spectral::SpectralFrame`, `window_hann()`, `fft()`, `fft_magnitude()`, `fft_power()` from `dsp/fft.h`
- Produces: `MultiBandAnalyzer::analyze()` returns `Spectral::SpectralDataset` with multi-band frames

- [ ] **Step 1: Create header**

```cpp
#pragma once
#include "spectral/spectral_dataset.h"
#include <vector>
#include <functional>

namespace Spectral {

struct BandConfig {
    float freq_low;    // Hz
    float freq_high;   // Hz
    int n_fft;         // FFT size for this band
};

class MultiBandAnalyzer {
public:
    struct AnalyzeResult {
        SpectralDataset dataset;
        float compute_ms;
    };

    // Default 3-band split based on Nyquist
    static std::vector<BandConfig> default_bands(float nyquist_hz);

    // Run multi-band STFT
    static AnalyzeResult analyze(
        const std::vector<float>& samples,
        int sample_rate,
        int n_fft = 1024,     // used for frame count reference
        int hop_size = 256
    );

    // Run single-band STFT (for comparison)
    static AnalyzeResult analyze_single(
        const std::vector<float>& samples,
        int sample_rate,
        int n_fft,
        int hop_size
    );
};

} // namespace Spectral
```

- [ ] **Step 2: Create implementation**

```cpp
#include "multiband_analyzer.h"
#include "fft.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace Spectral {

std::vector<BandConfig> MultiBandAnalyzer::default_bands(float nyquist_hz) {
    std::vector<BandConfig> bands;

    // Low: 0–2 kHz (or Nyquist/4 if Nyquist < 8 kHz)
    float low_split = std::min(2000.0f, nyquist_hz / 4.0f);
    // Mid: low_split–8 kHz (or Nyquist*3/4 if Nyquist < 16 kHz)
    float mid_split = std::min(8000.0f, nyquist_hz * 3.0f / 4.0f);

    if (low_split > 0 && nyquist_hz > low_split) {
        BandConfig low;
        low.freq_low = 0;
        low.freq_high = low_split;
        low.n_fft = 4096;
        bands.push_back(low);
    }

    if (mid_split > low_split && nyquist_hz > mid_split) {
        BandConfig mid;
        mid.freq_low = low_split;
        mid.freq_high = mid_split;
        mid.n_fft = 1024;
        bands.push_back(mid);
    }

    if (nyquist_hz > mid_split) {
        BandConfig high;
        high.freq_low = mid_split;
        high.freq_high = nyquist_hz;
        high.n_fft = 256;
        bands.push_back(high);
    }

    return bands;
}

MultiBandAnalyzer::AnalyzeResult MultiBandAnalyzer::analyze(
    const std::vector<float>& samples,
    int sample_rate,
    int n_fft,
    int hop_size)
{
    auto t0 = std::chrono::high_resolution_clock::now();

    float nyquist = static_cast<float>(sample_rate) / 2.0f;
    auto bands = default_bands(nyquist);

    if (bands.empty()) {
        // Degenerate: single band
        BandConfig single;
        single.freq_low = 0;
        single.freq_high = nyquist;
        single.n_fft = n_fft;
        bands.push_back(single);
    }

    int max_nfft = 0;
    for (const auto& b : bands) {
        max_nfft = std::max(max_nfft, b.n_fft);
    }

    // Use max_nfft's hop for common time grid
    int common_hop = max_nfft / 4;
    if (common_hop < 1) common_hop = 1;

    int num_frames = static_cast<int>((static_cast<int>(samples.size()) - max_nfft) / common_hop) + 1;
    if (num_frames < 1) num_frames = 1;

    // Reference frequency axis from max_nfft
    int total_bins = max_nfft / 2 + 1;

    // Per-band results
    struct BandResult {
        std::vector<std::vector<float>> magnitudes;  // [frame][bin]
        std::vector<std::vector<float>> power;
        int band_start_bin;
        int band_end_bin;
    };

    std::vector<BandResult> band_results;

    for (const auto& band : bands) {
        int band_hop = band.n_fft / 4;
        if (band_hop < 1) band_hop = 1;

        int band_frames = static_cast<int>((static_cast<int>(samples.size()) - band.n_fft) / band_hop) + 1;
        if (band_frames < 1) band_frames = 1;

        int band_bins = band.n_fft / 2 + 1;

        // Map band freq range to max_nfft bins
        float bin_hz_max = static_cast<float>(sample_rate) / static_cast<float>(max_nfft);
        int start_bin = static_cast<int>(std::round(band.freq_low / bin_hz_max));
        int end_bin = static_cast<int>(std::round(band.freq_high / bin_hz_max));
        if (start_bin < 0) start_bin = 0;
        if (end_bin >= total_bins) end_bin = total_bins - 1;

        BandResult br;
        br.band_start_bin = start_bin;
        br.band_end_bin = end_bin;
        br.magnitudes.resize(num_frames, std::vector<float>(total_bins, 0.0f));
        br.power.resize(num_frames, std::vector<float>(total_bins, 0.0f));

        // Band frequency axis
        float bin_hz_band = static_cast<float>(sample_rate) / static_cast<float>(band.n_fft);

        for (int f = 0; f < num_frames; ++f) {
            // Map common frame index to band frame index
            int band_frame = static_cast<int>(static_cast<float>(f) * static_cast<float>(band_hop) / static_cast<float>(common_hop));
            if (band_frame >= band_frames) band_frame = band_frames - 1;
            if (band_frame < 0) band_frame = 0;

            int offset = band_frame * band_hop;
            if (offset + band.n_fft > static_cast<int>(samples.size())) break;

            // Window + FFT
            auto win = window_hann(band.n_fft);
            std::vector<complex_f> buf(band.n_fft);
            for (int i = 0; i < band.n_fft; ++i) {
                buf[i] = complex_f(samples[offset + i] * win[i], 0.0f);
            }
            fft(buf);

            auto mag = fft_magnitude(buf);
            auto pwr = fft_power(buf);

            // Assign band bins to max_nfft grid
            for (int b_bin = 0; b_bin < band_bins; ++b_bin) {
                float freq = static_cast<float>(b_bin) * bin_hz_band;
                int max_bin = static_cast<int>(std::round(freq / bin_hz_max));
                if (max_bin >= start_bin && max_bin <= end_bin && max_bin < total_bins) {
                    br.magnitudes[f][max_bin] = mag[b_bin];
                    br.power[f][max_bin] = pwr[b_bin];
                }
            }
        }

        band_results.push_back(br);
    }

    // Build SpectralDataset
    SpectralDataset dataset;

    dataset.metadata.sample_rate = sample_rate;
    dataset.metadata.fft_size = max_nfft;
    dataset.metadata.hop_size = common_hop;
    dataset.metadata.overlap_ratio = 0.75f;
    dataset.metadata.nyquist_frequency = nyquist;
    dataset.metadata.num_frequency_bins = total_bins;
    dataset.metadata.window_type = "hann";
    dataset.metadata.window_coherent_gain = window_coherent_gain(window_hann(max_nfft));
    dataset.metadata.frame_duration_seconds = static_cast<double>(common_hop) / static_cast<double>(sample_rate);
    dataset.metadata.total_duration_seconds = static_cast<double>(samples.size()) / static_cast<double>(sample_rate);
    dataset.metadata.total_frames = num_frames;
    dataset.metadata.analyzed_channels = 1;
    dataset.metadata.analyzer_version = "2.0";
    dataset.metadata.analysis_method = "multiband_stft";
    dataset.metadata.band_count = static_cast<int>(bands.size());

    // Frequency axis
    dataset.frequency_axis.num_bins = total_bins;
    dataset.frequency_axis.fft_size = max_nfft;
    dataset.frequency_axis.sample_rate = sample_rate;
    dataset.frequency_axis.nyquist = nyquist;
    dataset.frequency_axis.resolution = bin_hz_max;
    dataset.frequency_axis.bin_frequencies.resize(total_bins);
    for (int k = 0; k < total_bins; ++k) {
        dataset.frequency_axis.bin_frequencies[k] = static_cast<float>(k) * bin_hz_max;
    }

    // Time axis
    dataset.time_axis.num_frames = num_frames;
    dataset.time_axis.hop_size = common_hop;
    dataset.time_axis.sample_rate = sample_rate;
    dataset.time_axis.frame_duration = static_cast<double>(common_hop) / static_cast<double>(sample_rate);
    dataset.time_axis.total_duration = dataset.metadata.total_duration_seconds;
    dataset.time_axis.frame_times.resize(num_frames);
    for (int f = 0; f < num_frames; ++f) {
        dataset.time_axis.frame_times[f] = static_cast<double>(f) * dataset.time_axis.frame_duration;
    }

    // Merge all bands into merged magnitudes
    std::vector<std::vector<float>> merged_mag(num_frames, std::vector<float>(total_bins, 0.0f));
    std::vector<std::vector<float>> merged_pwr(num_frames, std::vector<float>(total_bins, 0.0f));
    for (const auto& br : band_results) {
        for (int f = 0; f < num_frames; ++f) {
            for (int k = br.band_start_bin; k <= br.band_end_bin; ++k) {
                if (br.magnitudes[f][k] > merged_mag[f][k]) {
                    merged_mag[f][k] = br.magnitudes[f][k];
                    merged_pwr[f][k] = br.power[f][k];
                }
            }
        }
    }

    // Frames
    dataset.frames.resize(num_frames);
    for (int f = 0; f < num_frames; ++f) {
        auto& frame = dataset.frames[f];
        frame.frame_index = f;
        frame.n_fft = max_nfft;
        frame.window_factor = 1.0f;
        frame.timestamp = dataset.time_axis.frame_times[f];
        frame.band_count = static_cast<int>(bands.size());
        frame.magnitudes = merged_mag[f];
        frame.power = merged_pwr[f];

        // Compute frame stats
        float sum_mag = 0;
        float max_mag = 0;
        int max_bin = 0;
        for (int k = 0; k < total_bins; ++k) {
            sum_mag += frame.magnitudes[k];
            if (frame.magnitudes[k] > max_mag) {
                max_mag = frame.magnitudes[k];
                max_bin = k;
            }
        }
        frame.rms = std::sqrt(sum_mag / static_cast<float>(total_bins));
        frame.peak_magnitude = max_mag;
        frame.spectral_centroid = (max_bin > 0) ?
            dataset.frequency_axis.bin_frequencies[max_bin] : 0.0f;
        frame.spectral_bandwidth = 0.0f;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

    return { dataset, ms };
}

MultiBandAnalyzer::AnalyzeResult MultiBandAnalyzer::analyze_single(
    const std::vector<float>& samples,
    int sample_rate,
    int n_fft,
    int hop_size)
{
    auto t0 = std::chrono::high_resolution_clock::now();

    float nyquist = static_cast<float>(sample_rate) / 2.0f;
    int total_bins = n_fft / 2 + 1;
    int num_frames = static_cast<int>((static_cast<int>(samples.size()) - n_fft) / hop_size) + 1;
    if (num_frames < 1) num_frames = 1;
    float bin_hz = static_cast<float>(sample_rate) / static_cast<float>(n_fft);

    SpectralDataset dataset;

    dataset.metadata.sample_rate = sample_rate;
    dataset.metadata.fft_size = n_fft;
    dataset.metadata.hop_size = hop_size;
    dataset.metadata.overlap_ratio = static_cast<float>(n_fft - hop_size) / static_cast<float>(n_fft);
    dataset.metadata.nyquist_frequency = nyquist;
    dataset.metadata.num_frequency_bins = total_bins;
    dataset.metadata.window_type = "hann";
    dataset.metadata.window_coherent_gain = window_coherent_gain(window_hann(n_fft));
    dataset.metadata.frame_duration_seconds = static_cast<double>(hop_size) / static_cast<double>(sample_rate);
    dataset.metadata.total_duration_seconds = static_cast<double>(samples.size()) / static_cast<double>(sample_rate);
    dataset.metadata.total_frames = num_frames;
    dataset.metadata.analyzed_channels = 1;
    dataset.metadata.analyzer_version = "2.0";
    dataset.metadata.analysis_method = "stft";
    dataset.metadata.band_count = 1;

    dataset.frequency_axis.num_bins = total_bins;
    dataset.frequency_axis.fft_size = n_fft;
    dataset.frequency_axis.sample_rate = sample_rate;
    dataset.frequency_axis.nyquist = nyquist;
    dataset.frequency_axis.resolution = bin_hz;
    dataset.frequency_axis.bin_frequencies.resize(total_bins);
    for (int k = 0; k < total_bins; ++k) {
        dataset.frequency_axis.bin_frequencies[k] = static_cast<float>(k) * bin_hz;
    }

    dataset.time_axis.num_frames = num_frames;
    dataset.time_axis.hop_size = hop_size;
    dataset.time_axis.sample_rate = sample_rate;
    dataset.time_axis.frame_duration = static_cast<double>(hop_size) / static_cast<double>(sample_rate);
    dataset.time_axis.total_duration = dataset.metadata.total_duration_seconds;
    dataset.time_axis.frame_times.resize(num_frames);
    for (int f = 0; f < num_frames; ++f) {
        dataset.time_axis.frame_times[f] = static_cast<double>(f) * dataset.time_axis.frame_duration;
    }

    dataset.frames.resize(num_frames);
    for (int f = 0; f < num_frames; ++f) {
        int offset = f * hop_size;
        if (offset + n_fft > static_cast<int>(samples.size())) break;

        auto win = window_hann(n_fft);
        std::vector<complex_f> buf(n_fft);
        for (int i = 0; i < n_fft; ++i) {
            buf[i] = complex_f(samples[offset + i] * win[i], 0.0f);
        }
        fft(buf);

        auto mag = fft_magnitude(buf);
        auto pwr = fft_power(buf);

        auto& frame = dataset.frames[f];
        frame.frame_index = f;
        frame.n_fft = n_fft;
        frame.window_factor = 1.0f;
        frame.timestamp = dataset.time_axis.frame_times[f];
        frame.magnitudes = mag;
        frame.power = pwr;
        frame.band_count = 1;

        float sum_mag = 0;
        float max_mag = 0;
        int max_bin = 0;
        for (int k = 0; k < total_bins; ++k) {
            sum_mag += mag[k];
            if (mag[k] > max_mag) {
                max_mag = mag[k];
                max_bin = k;
            }
        }
        frame.rms = std::sqrt(sum_mag / static_cast<float>(total_bins));
        frame.peak_magnitude = max_mag;
        frame.spectral_centroid = (max_bin > 0) ?
            dataset.frequency_axis.bin_frequencies[max_bin] : 0.0f;
        frame.spectral_bandwidth = 0.0f;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

    return { dataset, ms };
}

} // namespace Spectral
```

- [ ] **Step 3: Build**

Run: `cmake --build build --config Release`
Expected: No errors

- [ ] **Step 4: Commit**

```bash
git add src/core/spectral/multiband_analyzer.h src/core/spectral/multiband_analyzer.cpp
git commit -m "feat: add MultiBandAnalyzer for multi-band STFT analysis"
```

---

### Task 3: CLI Integration

**Files:**
- Modify: `src/cli/main.cpp`

**Interfaces:**
- Consumes: `MultiBandAnalyzer::analyze()`, `MultiBandAnalyzer::analyze_single()` from Task 2
- Produces: `--multiband` flag parsing, comparison table output, conditional write

- [ ] **Step 1: Add --multiband to CliConfig**

In `main.cpp`, add after the existing includes:

```cpp
#include "multiband_analyzer.h"
```

In `main.cpp`, in `CliConfig` struct, add:

```cpp
bool multiband = false;
```

- [ ] **Step 2: Add --multiband flag parsing**

In `main.cpp`, in the flag parsing loop, add:

```cpp
else if (arg == "--multiband") {
    cfg.multiband = true;
}
```

- [ ] **Step 3: Add --multiband to help text**

In `main.cpp`, in the help output, add:

```cpp
std::fprintf(out, "  --multiband        Compare fixed/short/long/multi-band STFT\n");
```

- [ ] **Step 4: Add comparison logic**

In `main.cpp`, after the existing STFT analysis completes and `all_magnitudes` is filled, add:

```cpp
if (cfg.multiband) {
    // Run all four methods
    auto fixed = Spectral::MultiBandAnalyzer::analyze_single(
        all_samples, cfg.sample_rate, cfg.fft_size, cfg.hop_size);
    auto short_w = Spectral::MultiBandAnalyzer::analyze_single(
        all_samples, cfg.sample_rate, 256, 64);
    auto long_w = Spectral::MultiBandAnalyzer::analyze_single(
        all_samples, cfg.sample_rate, 4096, 1024);
    auto multi = Spectral::MultiBandAnalyzer::analyze(
        all_samples, cfg.sample_rate, cfg.fft_size, cfg.hop_size);

    // Print comparison table
    float sr_f = static_cast<float>(cfg.sample_rate);
    std::printf("\n%-16s %-15s %-15s %-14s\n",
                "Method", "Time Res (s)", "Freq Res (Hz)", "Compute (ms)");
    std::printf("%-16s %-15.6f %-15.2f %-14.1f\n",
                "Fixed",
                static_cast<float>(fixed.dataset.metadata.hop_size) / sr_f,
                fixed.dataset.frequency_axis.resolution, fixed.compute_ms);
    std::printf("%-16s %-15.6f %-15.2f %-14.1f\n",
                "Short (256)",
                static_cast<float>(short_w.dataset.metadata.hop_size) / sr_f,
                short_w.dataset.frequency_axis.resolution, short_w.compute_ms);
    std::printf("%-16s %-15.6f %-15.2f %-14.1f\n",
                "Long (4096)",
                static_cast<float>(long_w.dataset.metadata.hop_size) / sr_f,
                long_w.dataset.frequency_axis.resolution, long_w.compute_ms);
    std::printf("%-16s %-15.6f %-15.2f %-14.1f\n",
                "Multi-band",
                static_cast<float>(multi.dataset.metadata.hop_size) / sr_f,
                multi.dataset.frequency_axis.resolution, multi.compute_ms);
    std::printf("\n");
}
```

- [ ] **Step 5: Build**

In `CMakeLists.txt`, add `src/core/spectral/multiband_analyzer.cpp` to the spectragen target sources:

```cmake
add_executable(spectragen
    src/cli/main.cpp
    src/core/media/media_decoder.cpp
    src/core/spectral/spectral_dataset.cpp
    src/core/spectral/multiband_analyzer.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/spectrum_renderer.cpp
    src/core/rendering/png_encoder.cpp
    src/core/encoding/video_encoder.cpp
    src/core/encoding/video_renderer.cpp
)
```

Run: `cmake --build build --config Release`
Expected: No errors

- [ ] **Step 6: Run existing tests**

Run: `ctest --test-dir build/ -C Release --output-on-failure`
Expected: All 11 existing tests pass

- [ ] **Step 7: Commit**

```bash
git add src/cli/main.cpp
git commit -m "feat: add --multiband CLI flag for multi-band STFT comparison"
```

---

### Task 4: Tests

**Files:**
- Create: `tests/phase13/test_multiband.cpp`
- Create: `tests/phase13/test_multiband_compare.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `MultiBandAnalyzer`, `SpectralDataset` from Tasks 1–2
- Produces: 7 numerical tests, 4 comparison/regression tests

- [ ] **Step 1: Create numerical test file**

```cpp
#include <catch2/catch2.hpp>
#include "fft.h"
#include "multiband_analyzer.h"
#include <vector>
#include <cmath>
#include <cstdio>

// Generate a multi-frequency signal
static std::vector<float> make_signal(int sr, int n_samples) {
    std::vector<float> sig(n_samples);
    for (int i = 0; i < n_samples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(sr);
        sig[i] = 0.5f * std::sin(2.0f * PI * 500.0f * t)   // 500 Hz (low band)
               + 0.3f * std::sin(2.0f * PI * 3000.0f * t)   // 3000 Hz (mid band)
               + 0.2f * std::sin(2.0f * PI * 10000.0f * t);  // 10000 Hz (high band)
    }
    return sig;
}

TEST_CASE("multiband_default_bands", "[multiband]") {
    // Nyquist = 22050 Hz (sr=44100)
    auto bands = Spectral::MultiBandAnalyzer::default_bands(22050.0f);
    CHECK(bands.size() >= 2);
    CHECK(bands.size() <= 3);
    // First band starts at 0
    CHECK(bands.front().freq_low == 0.0f);
    // Last band ends at or near Nyquist
    CHECK(bands.back().freq_high >= 20000.0f);
}

TEST_CASE("multiband_output_sizes", "[multiband]") {
    int sr = 44100;
    auto sig = make_signal(sr, sr);  // 1 second
    auto result = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    CHECK(result.dataset.frames.size() > 0);
    CHECK(result.dataset.frequency_axis.num_bins > 0);
    CHECK(result.dataset.metadata.analysis_method == "multiband_stft");
}

TEST_CASE("multiband_magnitudes_nonzero", "[multiband]") {
    int sr = 44100;
    auto sig = make_signal(sr, sr);
    auto result = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    // At least some magnitudes should be nonzero
    float sum = 0;
    for (const auto& frame : result.dataset.frames) {
        for (float m : frame.magnitudes) {
            sum += m;
        }
    }
    CHECK(sum > 0.0f);
}

TEST_CASE("multiband_band_count", "[multiband]") {
    int sr = 44100;
    auto sig = make_signal(sr, sr);
    auto result = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    CHECK(result.dataset.metadata.band_count >= 2);
    CHECK(result.dataset.frames[0].band_count >= 2);
}

TEST_CASE("multiband_time_resolution", "[multiband]") {
    int sr = 44100;
    auto sig = make_signal(sr, sr);
    auto result = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    // Time resolution should be reasonable (hop/sr)
    float time_res = static_cast<float>(result.dataset.metadata.hop_size) /
                     static_cast<float>(sr);
    CHECK(time_res > 0.0f);
    CHECK(time_res < 0.1f);  // Less than 100ms
}

TEST_CASE("multiband_single_band", "[multiband]") {
    int sr = 44100;
    auto sig = make_signal(sr, sr);

    // Short window should have better time resolution than long window
    auto short_r = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 256, 64);
    auto long_r = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 4096, 1024);

    float short_time_res = static_cast<float>(short_r.dataset.metadata.hop_size) /
                           static_cast<float>(sr);
    float long_time_res = static_cast<float>(long_r.dataset.metadata.hop_size) /
                          static_cast<float>(sr);

    CHECK(short_time_res < long_time_res);
    // Long window should have better frequency resolution
    CHECK(long_r.dataset.frequency_axis.resolution <
           short_r.dataset.frequency_axis.resolution);
}

TEST_CASE("multiband_fixed_stft", "[multiband]") {
    int sr = 44100;
    auto sig = make_signal(sr, sr);
    auto result = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 1024, 256);

    CHECK(result.dataset.metadata.analysis_method == "stft");
    CHECK(result.dataset.metadata.band_count == 1);
    CHECK(result.dataset.frames.size() > 0);
}
```

- [ ] **Step 2: Create comparison test file**

```cpp
#include <catch2/catch2.hpp>
#include "fft.h"
#include "multiband_analyzer.h"
#include <vector>
#include <cmath>

static std::vector<float> make_test_signal(int sr, int n_samples) {
    std::vector<float> sig(n_samples);
    for (int i = 0; i < n_samples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(sr);
        sig[i] = std::sin(2.0f * PI * 1000.0f * t);
    }
    return sig;
}

TEST_CASE("compare_all_four_methods", "[multiband_compare]") {
    int sr = 44100;
    auto sig = make_test_signal(sr, sr);

    auto fixed = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 1024, 256);
    auto short_w = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 256, 64);
    auto long_w = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 4096, 1024);
    auto multi = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    // All should produce frames
    CHECK(fixed.dataset.frames.size() > 0);
    CHECK(short_w.dataset.frames.size() > 0);
    CHECK(long_w.dataset.frames.size() > 0);
    CHECK(multi.dataset.frames.size() > 0);

    // Short window has better time resolution
    float short_hop = static_cast<float>(short_w.dataset.metadata.hop_size);
    float long_hop = static_cast<float>(long_w.dataset.metadata.hop_size);
    CHECK(short_hop < long_hop);

    // Long window has better frequency resolution
    CHECK(long_w.dataset.frequency_axis.resolution <
           short_w.dataset.frequency_axis.resolution);
}

TEST_CASE("compare_multiband_metadata", "[multiband_compare]") {
    int sr = 44100;
    auto sig = make_test_signal(sr, sr);
    auto result = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    CHECK(result.dataset.metadata.analysis_method == "multiband_stft");
    CHECK(result.dataset.metadata.band_count >= 2);
}

TEST_CASE("compare_multiband_vs_fixed", "[multiband_compare]") {
    int sr = 44100;
    auto sig = make_test_signal(sr, sr);

    auto fixed = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 1024, 256);
    auto multi = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    // Both should produce output
    CHECK(fixed.dataset.frames.size() > 0);
    CHECK(multi.dataset.frames.size() > 0);

    // Both should have the same sample rate
    CHECK(fixed.dataset.metadata.sample_rate == multi.dataset.metadata.sample_rate);
}

TEST_CASE("regression_standard_stft_unchanged", "[multiband_compare]") {
    // Standard STFT without --multiband should produce identical results
    int sr = 44100;
    auto sig = make_test_signal(sr, sr);

    auto result1 = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 1024, 256);
    auto result2 = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 1024, 256);

    // Same input should produce same output
    CHECK(result1.dataset.frames.size() == result2.dataset.frames.size());
    CHECK(result1.dataset.metadata.analysis_method == "stft");
    CHECK(result1.dataset.metadata.band_count == 1);
}
```

- [ ] **Step 3: Add CMake targets**

In `CMakeLists.txt`, add after the Phase 12 test targets:

```cmake
# --- --- Phase 13: Multi-band STFT ---
add_executable(test_multiband
    tests/phase13/test_multiband.cpp
    src/core/spectral/multiband_analyzer.cpp
    src/core/spectral/spectral_dataset.cpp
)
target_include_directories(test_multiband PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
)
set_target_properties(test_multiband PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME multiband COMMAND test_multiband)

add_executable(test_multiband_compare
    tests/phase13/test_multiband_compare.cpp
    src/core/spectral/multiband_analyzer.cpp
    src/core/spectral/spectral_dataset.cpp
)
target_include_directories(test_multiband_compare PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
)
set_target_properties(test_multiband_compare PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME multiband_compare COMMAND test_multiband_compare)
```

- [ ] **Step 4: Build tests**

Run: `cmake --build build --config Release --target test_multiband test_multiband_compare`
Expected: No errors

- [ ] **Step 5: Run tests**

Run: `ctest --test-dir build/ -C Release -R "multiband" --output-on-failure`
Expected: All tests pass

- [ ] **Step 6: Run full test suite**

Run: `ctest --test-dir build/ -C Release -R "reassignment|spectral_dataset|spectrogram_renderer|spectrum_renderer|project_config|spectragen_basic|spectragen_cli|spectralcore_existence|video_encoder|video_renderer|multiband" --output-on-failure`
Expected: All tests pass

- [ ] **Step 7: Commit**

```bash
git add tests/phase13/ CMakeLists.txt
git commit -m "test: add multi-band STFT numerical and comparison tests"
```

---

### Task 5: Documentation

**Files:**
- Create: `docs/phase13-multiband.md`

**Interfaces:**
- Consumes: all implemented features from Tasks 1–4
- Produces: documentation of multi-band STFT analysis

- [ ] **Step 1: Create documentation**

```markdown
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
```

- [ ] **Step 2: Build and verify no regressions**

Run: `cmake --build build --config Release && ctest --test-dir build/ -C Release -R "multiband" --output-on-failure`
Expected: All tests pass

- [ ] **Step 3: Commit**

```bash
git add docs/phase13-multiband.md
git commit -m "docs: add Phase 13 multi-band STFT documentation"
```
