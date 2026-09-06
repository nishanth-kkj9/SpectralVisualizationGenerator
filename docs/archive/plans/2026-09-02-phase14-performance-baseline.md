# Phase 14: Performance Baseline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a benchmarking suite that measures wall time, throughput, peak memory, and CPU utilization across the spectral visualization pipeline, saving results as JSON.

**Architecture:** Single executable (`spectral_benchmarks`) with a harness header providing timing/memory primitives. Each benchmark is a separate .cpp file registering itself. Main runner iterates the parameter matrix and outputs JSON.

**Tech Stack:** C++20, `std::chrono`, Win32 `GetProcessMemoryInfo`/`GetProcessTimes`, no external dependencies.

**Spec:** `docs/superpowers/specs/2026-09-02-phase14-performance-baseline-design.md`
**Plan:** `docs/superpowers/plans/2026-09-02-phase14-performance-baseline.md`

## Global Constraints

- C++20, MSVC 19.51, Windows
- Build: `cmake --build build --config Release`
- All LSP errors are spurious — real build succeeds via cmake
- `fft.h` is in `src/core/dsp/` — include as `#include "fft.h"`
- `SpectralDataset` uses private members with mutable accessors
- `MultiBandAnalyzer::analyze_single()` returns `AnalyzeResult { SpectralDataset, float compute_ms }`
- No Google Benchmark — custom harness with `std::chrono`
- No external test files — all data generated synthetically

---

## File Map

| File | Responsibility |
|------|---------------|
| `src/benchmarks/benchmark_harness.h` | Timing, memory, CPU measurement; audio/WAV generation |
| `src/benchmarks/bench_fft.cpp` | FFT benchmarks across sizes |
| `src/benchmarks/bench_stft.cpp` | STFT benchmarks via MultiBandAnalyzer |
| `src/benchmarks/bench_dataset.cpp` | SpectralDataset serialize/deserialize |
| `src/benchmarks/bench_render.cpp` | SpectrogramRenderer + PNG encoding |
| `src/benchmarks/bench_audio.cpp` | AudioBuffer mono mixdown |
| `src/benchmarks/bench_media.cpp` | WAV decode via MediaDecoder |
| `src/benchmarks/bench_video.cpp` | VideoRenderer frame rendering |
| `src/benchmarks/main.cpp` | Runner, matrix iteration, JSON output |
| `CMakeLists.txt` | Update spectral_benchmarks target |

---

### Task 1: Benchmark Harness

**Files:**
- Create: `src/benchmarks/benchmark_harness.h`

**Interfaces:**
- Produces: `bench::Result`, `bench::peak_memory_mb()`, `bench::cpu_time_ms()`, `bench::bench_fn()`, `bench::generate_audio()`, `bench::generate_wav()`

- [ ] **Step 1: Create benchmark_harness.h**

```cpp
#pragma once
#include <chrono>
#include <vector>
#include <string>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <numeric>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace bench {

struct Result {
    std::string benchmark;
    int sample_rate = 0;
    int fft_size = 0;
    int duration_sec = 0;
    std::string extra;
    double wall_ms = 0.0;
    double throughput_mbs = 0.0;
    double peak_memory_mb = 0.0;
    double cpu_time_ms = 0.0;
    int iterations = 0;
};

inline double peak_memory_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
#endif
    return 0.0;
}

inline double cpu_time_ms() {
#ifdef _WIN32
    FILETIME creation, exit_ft, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_ft, &kernel, &user)) {
        auto to_ms = [](const FILETIME& ft) -> double {
            ULARGE_INTEGER li;
            li.LowPart = ft.dwLowDateTime;
            li.HighPart = ft.dwHighDateTime;
            return static_cast<double>(li.QuadPart) / 10000.0; // 100ns ticks to ms
        };
        return to_ms(kernel) + to_ms(user);
    }
#endif
    return 0.0;
}

inline double bench_fn(std::function<void()> fn, int iterations = 3) {
    // Warmup
    fn();

    std::vector<double> times;
    for (int i = 0; i < iterations; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();
        fn();
        auto t1 = std::chrono::high_resolution_clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2]; // median
}

inline std::vector<float> generate_audio(int sample_rate, int num_samples) {
    std::vector<float> samples(num_samples);
    const float freqs[] = {440.0f, 880.0f, 1760.0f, 3520.0f};
    const float amps[] = {0.25f, 0.25f, 0.25f, 0.25f};
    for (int i = 0; i < num_samples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(sample_rate);
        float s = 0.0f;
        for (int f = 0; f < 4; ++f) {
            s += amps[f] * std::sin(2.0f * 3.14159265f * freqs[f] * t);
        }
        samples[i] = s;
    }
    return samples;
}

inline std::string generate_wav(const std::string& path, int sample_rate, int num_samples) {
    auto samples = generate_audio(sample_rate, num_samples);

    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "wb");
    if (!f) return "";

    // WAV header
    int num_channels = 1;
    int bits_per_sample = 16;
    int byte_rate = sample_rate * num_channels * bits_per_sample / 8;
    int block_align = num_channels * bits_per_sample / 8;
    int data_size = num_samples * block_align;

    fwrite("RIFF", 1, 4, f);
    uint32_t chunk_size = 36 + data_size;
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_format = 1; // PCM
    fwrite(&audio_format, 2, 1, f);
    uint16_t ch = static_cast<uint16_t>(num_channels);
    fwrite(&ch, 2, 1, f);
    uint32_t sr = static_cast<uint32_t>(sample_rate);
    fwrite(&sr, 4, 1, f);
    uint32_t br = static_cast<uint32_t>(byte_rate);
    fwrite(&br, 4, 1, f);
    uint16_t ba = static_cast<uint16_t>(block_align);
    fwrite(&ba, 2, 1, f);
    uint16_t bps = static_cast<uint16_t>(bits_per_sample);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    uint32_t ds = static_cast<uint32_t>(data_size);
    fwrite(&ds, 4, 1, f);

    // Write samples as 16-bit PCM
    for (int i = 0; i < num_samples; ++i) {
        float clamped = std::max(-1.0f, std::min(1.0f, samples[i]));
        int16_t s = static_cast<int16_t>(clamped * 32767.0f);
        fwrite(&s, 2, 1, f);
    }

    fclose(f);
    return path;
}

} // namespace bench
```

- [ ] **Step 2: Verify header compiles**

Create a minimal test file that includes the header:
```cpp
// src/benchmarks/test_harness.cpp
#include "benchmark_harness.h"
int main() {
    bench::Result r;
    r.benchmark = "test";
    auto audio = bench::generate_audio(44100, 44100);
    return audio.empty() ? 1 : 0;
}
```

Add to CMakeLists.txt temporarily, build, verify it compiles.

- [ ] **Step 3: Commit**

```bash
git add src/benchmarks/benchmark_harness.h
git commit -m "bench: add benchmark harness with timing, memory, and audio generation"
```

---

### Task 2: FFT Benchmark

**Files:**
- Create: `src/benchmarks/bench_fft.cpp`

**Interfaces:**
- Consumes: `bench::Result`, `bench::bench_fn()`, `bench::peak_memory_mb()`, `bench::cpu_time_ms()`, `fft()` from `fft.h`
- Produces: `bench_fft(int sample_rate, int fft_size, int duration_sec)` returning `bench::Result`

- [ ] **Step 1: Create bench_fft.cpp**

```cpp
#include "benchmark_harness.h"
#include "fft.h"
#include <vector>

bench::Result bench_fft(int sample_rate, int fft_size, int duration_sec) {
    int total_samples = sample_rate * duration_sec;
    int num_frames = total_samples / fft_size;
    if (num_frames < 1) num_frames = 1;

    // Pre-generate all frames
    std::vector<std::vector<complex_f>> frames(num_frames, std::vector<complex_f>(fft_size));
    for (int f = 0; f < num_frames; ++f) {
        auto audio = bench::generate_audio(sample_rate, fft_size);
        for (int i = 0; i < fft_size; ++i) {
            frames[f][i] = complex_f(audio[i], 0.0f);
        }
    }

    double mem_before = bench::peak_memory_mb();
    double cpu_before = bench::cpu_time_ms();

    double wall_ms = bench::bench_fn([&]() {
        for (int f = 0; f < num_frames; ++f) {
            fft(frames[f]);
        }
    });

    double mem_after = bench::peak_memory_mb();
    double cpu_after = bench::cpu_time_ms();

    int64_t total_bytes = static_cast<int64_t>(num_frames) * fft_size * sizeof(complex_f);

    bench::Result r;
    r.benchmark = "fft";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;
    r.wall_ms = wall_ms;
    r.throughput_mbs = (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / (wall_ms / 1000.0);
    r.peak_memory_mb = mem_after;
    r.cpu_time_ms = cpu_after - cpu_before;
    r.iterations = 3;
    return r;
}
```

- [ ] **Step 2: Verify it compiles**

Temporarily add to CMakeLists.txt, build, verify.

- [ ] **Step 3: Commit**

```bash
git add src/benchmarks/bench_fft.cpp
git commit -m "bench: add FFT benchmark across sizes and sample rates"
```

---

### Task 3: STFT Benchmark

**Files:**
- Create: `src/benchmarks/bench_stft.cpp`

**Interfaces:**
- Consumes: `MultiBandAnalyzer::analyze_single()` from `multiband_analyzer.h`
- Produces: `bench_stft(int sample_rate, int fft_size, int duration_sec)` returning `bench::Result`

- [ ] **Step 1: Create bench_stft.cpp**

```cpp
#include "benchmark_harness.h"
#include "multiband_analyzer.h"
#include <vector>

bench::Result bench_stft(int sample_rate, int fft_size, int duration_sec) {
    int total_samples = sample_rate * duration_sec;
    int hop_size = fft_size / 4;
    auto samples = bench::generate_audio(sample_rate, total_samples);

    double mem_before = bench::peak_memory_mb();
    double cpu_before = bench::cpu_time_ms();

    Spectral::MultiBandAnalyzer::AnalyzeResult result;
    double wall_ms = bench::bench_fn([&]() {
        result = Spectral::MultiBandAnalyzer::analyze_single(samples, sample_rate, fft_size, hop_size);
    });

    double mem_after = bench::peak_memory_mb();
    double cpu_after = bench::cpu_time_ms();

    int64_t total_bytes = static_cast<int64_t>(total_samples) * sizeof(float);

    bench::Result r;
    r.benchmark = "stft";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;
    r.wall_ms = wall_ms;
    r.throughput_mbs = (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / (wall_ms / 1000.0);
    r.peak_memory_mb = mem_after;
    r.cpu_time_ms = cpu_after - cpu_before;
    r.iterations = 3;
    return r;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/benchmarks/bench_stft.cpp
git commit -m "bench: add STFT benchmark via MultiBandAnalyzer"
```

---

### Task 4: Dataset Benchmark

**Files:**
- Create: `src/benchmarks/bench_dataset.cpp`

**Interfaces:**
- Consumes: `SpectralDataset`, `MultiBandAnalyzer::analyze_single()` from Tasks 2-3
- Produces: `bench_dataset(int sample_rate, int duration_sec)` returning `bench::Result`

- [ ] **Step 1: Create bench_dataset.cpp**

```cpp
#include "benchmark_harness.h"
#include "multiband_analyzer.h"
#include <vector>
#include <cstdio>

bench::Result bench_dataset(int sample_rate, int duration_sec) {
    int total_samples = sample_rate * duration_sec;
    int fft_size = 4096;
    int hop_size = fft_size / 4;
    auto samples = bench::generate_audio(sample_rate, total_samples);

    // Generate dataset once
    auto result = Spectral::MultiBandAnalyzer::analyze_single(samples, sample_rate, fft_size, hop_size);
    auto& dataset = result.dataset;

    // Benchmark serialize
    double mem_before = bench::peak_memory_mb();
    double cpu_before = bench::cpu_time_ms();

    double wall_ms = bench::bench_fn([&]() {
        dataset.save_to_file("bench_temp.dat");
    });

    // Benchmark deserialize
    double wall_ms2 = bench::bench_fn([&]() {
        Spectral::SpectralDataset loaded;
        loaded.load_from_file("bench_temp.dat");
    });

    // Cleanup
    std::remove("bench_temp.dat");

    double mem_after = bench::peak_memory_mb();
    double cpu_after = bench::cpu_time_ms();

    int64_t frame_count = dataset.frame_count();
    int64_t estimated_size = frame_count * (fft_size / 2 + 1) * sizeof(float) * 2;

    bench::Result r;
    r.benchmark = "dataset";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;
    r.wall_ms = wall_ms + wall_ms2;
    r.throughput_mbs = (static_cast<double>(estimated_size) / (1024.0 * 1024.0)) / (r.wall_ms / 1000.0);
    r.peak_memory_mb = mem_after;
    r.cpu_time_ms = cpu_after - cpu_before;
    r.iterations = 3;
    return r;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/benchmarks/bench_dataset.cpp
git commit -m "bench: add SpectralDataset serialization benchmark"
```

---

### Task 5: Render + PNG Benchmark

**Files:**
- Create: `src/benchmarks/bench_render.cpp`

**Interfaces:**
- Consumes: `SpectrogramRenderer`, `PNGEncoder` from `spectrogram_renderer.h`, `png_encoder.h`
- Produces: `bench_render(int width, int height)` returning `bench::Result`

- [ ] **Step 1: Create bench_render.cpp**

```cpp
#include "benchmark_harness.h"
#include "multiband_analyzer.h"
#include "spectrogram_renderer.h"
#include "png_encoder.h"
#include <vector>
#include <cstdio>

bench::Result bench_render(int width, int height) {
    // Generate a small dataset for rendering
    int sr = 44100;
    int duration = 10; // 10 seconds
    int total_samples = sr * duration;
    auto samples = bench::generate_audio(sr, total_samples);
    auto result = Spectral::MultiBandAnalyzer::analyze_single(samples, sr, 4096, 1024);

    Spectral::SpectrogramConfig config;
    config.width = width;
    config.height = height;

    Spectral::SpectrogramRenderer renderer(config);

    double mem_before = bench::peak_memory_mb();
    double cpu_before = bench::cpu_time_ms();

    // Benchmark rendering
    double wall_ms = bench::bench_fn([&]() {
        Spectral::RGBAImage image;
        renderer.render(result.dataset, image);
    });

    // Benchmark PNG encoding
    Spectral::RGBAImage image;
    renderer.render(result.dataset, image);

    double wall_ms2 = bench::bench_fn([&]() {
        Spectral::PNGEncoder::write_rgba("bench_render.png", width, height, image.pixels);
    });

    // Cleanup
    std::remove("bench_render.png");

    double mem_after = bench::peak_memory_mb();
    double cpu_after = bench::cpu_time_ms();

    int64_t pixel_bytes = static_cast<int64_t>(width) * height * 4;

    bench::Result r;
    r.benchmark = "render";
    r.extra = std::to_string(width) + "x" + std::to_string(height);
    r.wall_ms = wall_ms + wall_ms2;
    r.throughput_mbs = (static_cast<double>(pixel_bytes) / (1024.0 * 1024.0)) / (r.wall_ms / 1000.0);
    r.peak_memory_mb = mem_after;
    r.cpu_time_ms = cpu_after - cpu_before;
    r.iterations = 3;
    return r;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/benchmarks/bench_render.cpp
git commit -m "bench: add rendering and PNG encoding benchmark"
```

---

### Task 6: Audio Benchmark

**Files:**
- Create: `src/benchmarks/bench_audio.cpp`

**Interfaces:**
- Consumes: `AudioBuffer::mix_down()` from `audio_buffer.h`
- Produces: `bench_audio(int sample_rate, int duration_sec)` returning `bench::Result`

- [ ] **Step 1: Create bench_audio.cpp**

```cpp
#include "benchmark_harness.h"
#include "audio_buffer.h"
#include <vector>

bench::Result bench_audio(int sample_rate, int duration_sec) {
    int total_samples = sample_rate * duration_sec;
    int num_channels = 2;

    // Create stereo interleaved buffer
    AudioBuffer buffer;
    buffer.sample_rate = sample_rate;
    buffer.num_channels = num_channels;
    auto mono = bench::generate_audio(sample_rate, total_samples);
    buffer.data.resize(total_samples * num_channels);
    for (int i = 0; i < total_samples; ++i) {
        buffer.data[i * 2] = mono[i];
        buffer.data[i * 2 + 1] = mono[i] * 0.8f; // slightly different L/R
    }

    double mem_before = bench::peak_memory_mb();
    double cpu_before = bench::cpu_time_ms();

    double wall_ms = bench::bench_fn([&]() {
        buffer.mix_down();
    });

    double mem_after = bench::peak_memory_mb();
    double cpu_after = bench::cpu_time_ms();

    int64_t total_bytes = static_cast<int64_t>(total_samples) * num_channels * sizeof(float);

    bench::Result r;
    r.benchmark = "audio";
    r.sample_rate = sample_rate;
    r.duration_sec = duration_sec;
    r.wall_ms = wall_ms;
    r.throughput_mbs = (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / (wall_ms / 1000.0);
    r.peak_memory_mb = mem_after;
    r.cpu_time_ms = cpu_after - cpu_before;
    r.iterations = 3;
    return r;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/benchmarks/bench_audio.cpp
git commit -m "bench: add audio conversion benchmark"
```

---

### Task 7: Media Decode Benchmark

**Files:**
- Create: `src/benchmarks/bench_media.cpp`

**Interfaces:**
- Consumes: `MediaDecoder` from `media_decoder.h`
- Produces: `bench_media(int sample_rate, int duration_sec)` returning `bench::Result`

- [ ] **Step 1: Create bench_media.cpp**

```cpp
#include "benchmark_harness.h"
#include "media_decoder.h"
#include <cstdio>

bench::Result bench_media(int sample_rate, int duration_sec) {
    int total_samples = sample_rate * duration_sec;

    // Generate WAV file
    std::string wav_path = "bench_media_" + std::to_string(sample_rate) + ".wav";
    bench::generate_wav(wav_path, sample_rate, total_samples);

    double mem_before = bench::peak_memory_mb();
    double cpu_before = bench::cpu_time_ms();

    double wall_ms = bench::bench_fn([&]() {
        MediaDecoder decoder;
        decoder.open(wav_path);
        AudioFrame frame;
        while (decoder.read_frame(frame)) {
            // consume frames
        }
        decoder.close();
    });

    double mem_after = bench::peak_memory_mb();
    double cpu_after = bench::cpu_time_ms();

    std::remove(wav_path.c_str());

    int64_t total_bytes = static_cast<int64_t>(total_samples) * sizeof(int16_t);

    bench::Result r;
    r.benchmark = "media_decode";
    r.sample_rate = sample_rate;
    r.duration_sec = duration_sec;
    r.wall_ms = wall_ms;
    r.throughput_mbs = (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / (wall_ms / 1000.0);
    r.peak_memory_mb = mem_after;
    r.cpu_time_ms = cpu_after - cpu_before;
    r.iterations = 3;
    return r;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/benchmarks/bench_media.cpp
git commit -m "bench: add media decode benchmark (WAV via ffmpeg)"
```

---

### Task 8: Video Benchmark

**Files:**
- Create: `src/benchmarks/bench_video.cpp`

**Interfaces:**
- Consumes: `VideoRenderer::render_frame()` from `video_renderer.h`
- Produces: `bench_video(int width, int height)` returning `bench::Result`

- [ ] **Step 1: Create bench_video.cpp**

```cpp
#include "benchmark_harness.h"
#include "multiband_analyzer.h"
#include "video_renderer.h"
#include <vector>

bench::Result bench_video(int width, int height) {
    // Generate dataset
    int sr = 44100;
    int duration = 10;
    int total_samples = sr * duration;
    auto samples = bench::generate_audio(sr, total_samples);
    auto result = Spectral::MultiBandAnalyzer::analyze_single(samples, sr, 4096, 1024);

    Spectral::VideoRendererConfig config;
    config.width = width;
    config.height = height;
    config.fps = 30;

    Spectral::VideoRenderer renderer(config);

    double mem_before = bench::peak_memory_mb();
    double cpu_before = bench::cpu_time_ms();

    // Benchmark single frame rendering
    double wall_ms = bench::bench_fn([&]() {
        Spectral::RGBAImage frame;
        renderer.render_frame(result.dataset, 5.0, frame);
    });

    double mem_after = bench::peak_memory_mb();
    double cpu_after = bench::cpu_time_ms();

    int64_t frame_bytes = static_cast<int64_t>(width) * height * 4;

    bench::Result r;
    r.benchmark = "video_render";
    r.extra = std::to_string(width) + "x" + std::to_string(height);
    r.wall_ms = wall_ms;
    r.throughput_mbs = (static_cast<double>(frame_bytes) / (1024.0 * 1024.0)) / (wall_ms / 1000.0);
    r.peak_memory_mb = mem_after;
    r.cpu_time_ms = cpu_after - cpu_before;
    r.iterations = 3;
    return r;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/benchmarks/bench_video.cpp
git commit -m "bench: add video rendering benchmark"
```

---

### Task 9: Main Runner + CMake

**Files:**
- Modify: `src/benchmarks/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: all bench_* functions from Tasks 2-8
- Produces: `spectral_benchmarks` executable, JSON output

- [ ] **Step 1: Update main.cpp**

```cpp
#include "benchmark_harness.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>

// Forward declarations from bench_*.cpp files
bench::Result bench_fft(int sample_rate, int fft_size, int duration_sec);
bench::Result bench_stft(int sample_rate, int fft_size, int duration_sec);
bench::Result bench_dataset(int sample_rate, int duration_sec);
bench::Result bench_render(int width, int height);
bench::Result bench_audio(int sample_rate, int duration_sec);
bench::Result bench_media(int sample_rate, int duration_sec);
bench::Result bench_video(int width, int height);

static void write_json(const std::vector<bench::Result>& results, const char* path) {
    FILE* f = nullptr;
    fopen_s(&f, path, "w");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return; }
    fprintf(f, "[\n");
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        fprintf(f, "  {\"benchmark\":\"%s\",\"sample_rate\":%d,\"fft_size\":%d,\"duration_sec\":%d,"
                "\"extra\":\"%s\",\"wall_ms\":%.2f,\"throughput_mbs\":%.2f,"
                "\"peak_memory_mb\":%.2f,\"cpu_time_ms\":%.2f,\"iterations\":%d}",
                r.benchmark.c_str(), r.sample_rate, r.fft_size, r.duration_sec,
                r.extra.c_str(), r.wall_ms, r.throughput_mbs,
                r.peak_memory_mb, r.cpu_time_ms, r.iterations);
        if (i < results.size() - 1) fprintf(f, ",\n");
    }
    fprintf(f, "\n]\n");
    fclose(f);
    printf("Results written to %s\n", path);
}

int main(int argc, char* argv[]) {
    bool quick = false;
    const char* output = "benchmark_results.json";
    std::string filter;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--quick") == 0) quick = true;
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) output = argv[++i];
        else if (strcmp(argv[i], "--benchmark") == 0 && i + 1 < argc) filter = argv[++i];
    }

    int sample_rates[] = {44100, 48000, 96000};
    int fft_sizes[] = {2048, 4096, 8192, 16384, 32768};
    int durations[] = {60, 600};
    if (!quick) {
        // Add 1-hour for non-quick mode (only for stft and dataset)
    }
    int resolutions[][2] = {{1024, 512}, {1920, 1080}, {3840, 2160}};

    std::vector<bench::Result> results;

    auto run = [&](const std::string& name, bool should_run) {
        return filter.empty() || filter == name || should_run;
    };

    printf("=== Phase 14: Performance Baseline ===\n");
    printf("Mode: %s\n", quick ? "quick (1min only)" : "full");
    printf("Output: %s\n\n", output);

    // FFT benchmarks
    if (run("fft", true)) {
        printf("Running FFT benchmarks...\n");
        for (int sr : sample_rates) {
            for (int nfft : fft_sizes) {
                for (int dur : durations) {
                    printf("  FFT sr=%d nfft=%d dur=%ds...", sr, nfft, dur);
                    auto r = bench_fft(sr, nfft, dur);
                    printf(" %.1fms %.1f MB/s\n", r.wall_ms, r.throughput_mbs);
                    results.push_back(r);
                }
            }
        }
    }

    // STFT benchmarks
    if (run("stft", true)) {
        printf("Running STFT benchmarks...\n");
        for (int sr : sample_rates) {
            for (int nfft : fft_sizes) {
                for (int dur : durations) {
                    printf("  STFT sr=%d nfft=%d dur=%ds...", sr, nfft, dur);
                    auto r = bench_stft(sr, nfft, dur);
                    printf(" %.1fms %.1f MB/s\n", r.wall_ms, r.throughput_mbs);
                    results.push_back(r);
                }
            }
        }
    }

    // Dataset benchmarks
    if (run("dataset", true)) {
        printf("Running Dataset benchmarks...\n");
        for (int sr : sample_rates) {
            for (int dur : durations) {
                printf("  Dataset sr=%d dur=%ds...", sr, dur);
                auto r = bench_dataset(sr, dur);
                printf(" %.1fms\n", r.wall_ms);
                results.push_back(r);
            }
        }
    }

    // Render benchmarks
    if (run("render", true)) {
        printf("Running Render benchmarks...\n");
        for (auto& res : resolutions) {
            printf("  Render %dx%d...", res[0], res[1]);
            auto r = bench_render(res[0], res[1]);
            printf(" %.1fms\n", r.wall_ms);
            results.push_back(r);
        }
    }

    // Audio benchmarks
    if (run("audio", true)) {
        printf("Running Audio benchmarks...\n");
        for (int sr : sample_rates) {
            for (int dur : durations) {
                printf("  Audio sr=%d dur=%ds...", sr, dur);
                auto r = bench_audio(sr, dur);
                printf(" %.1fms\n", r.wall_ms);
                results.push_back(r);
            }
        }
    }

    // Media decode benchmarks
    if (run("media_decode", true)) {
        printf("Running Media Decode benchmarks...\n");
        for (int sr : sample_rates) {
            for (int dur : durations) {
                printf("  Media sr=%d dur=%ds...", sr, dur);
                auto r = bench_media(sr, dur);
                printf(" %.1fms\n", r.wall_ms);
                results.push_back(r);
            }
        }
    }

    // Video benchmarks
    if (run("video_render", true)) {
        printf("Running Video benchmarks...\n");
        for (auto& res : resolutions) {
            printf("  Video %dx%d...", res[0], res[1]);
            auto r = bench_video(res[0], res[1]);
            printf(" %.1fms\n", r.wall_ms);
            results.push_back(r);
        }
    }

    printf("\n=== Complete: %zu benchmark results ===\n", results.size());
    write_json(results, output);
    return 0;
}
```

- [ ] **Step 2: Update CMakeLists.txt**

Replace the existing `spectral_benchmarks` target (lines 79-92) with:

```cmake
add_executable(spectral_benchmarks
    src/benchmarks/main.cpp
    src/benchmarks/bench_fft.cpp
    src/benchmarks/bench_stft.cpp
    src/benchmarks/bench_dataset.cpp
    src/benchmarks/bench_render.cpp
    src/benchmarks/bench_audio.cpp
    src/benchmarks/bench_media.cpp
    src/benchmarks/bench_video.cpp
    src/core/spectral/multiband_analyzer.cpp
    src/core/spectral/spectral_dataset.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/spectrum_renderer.cpp
    src/core/rendering/png_encoder.cpp
    src/core/encoding/video_encoder.cpp
    src/core/encoding/video_renderer.cpp
    src/core/media/media_decoder.cpp
)
target_include_directories(spectral_benchmarks PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/media>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
)
target_link_libraries(spectral_benchmarks PRIVATE SpectralCore)

set_target_properties(spectral_benchmarks PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)

set_target_properties(spectral_benchmarks PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:CONSOLE"
)
```

- [ ] **Step 3: Build and verify**

```bash
cmake --build build --config Release --target spectral_benchmarks
```

Expected: No errors.

- [ ] **Step 4: Commit**

```bash
git add src/benchmarks/main.cpp src/benchmarks/bench_*.cpp CMakeLists.txt
git commit -m "bench: add main runner with parameter matrix and JSON output"
```

---

### Task 10: Run Baseline & Save

**Files:**
- Create: `benchmarks/2026-09-02-baseline.json` (output)

**Interfaces:**
- Consumes: `spectral_benchmarks` executable
- Produces: saved baseline JSON

- [ ] **Step 1: Run quick baseline**

```bash
build/Release/spectral_benchmarks.exe --quick --output benchmarks/2026-09-02-baseline-quick.json
```

Verify it completes and produces JSON.

- [ ] **Step 2: Run full baseline**

```bash
build/Release/spectral_benchmarks.exe --output benchmarks/2026-09-02-baseline.json
```

- [ ] **Step 3: Commit baseline**

```bash
git add benchmarks/
git commit -m "bench: save Phase 14 performance baseline"
```

- [ ] **Step 4: Report results**

Report back with a summary table of key findings.
