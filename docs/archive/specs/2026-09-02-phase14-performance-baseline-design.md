# Phase 14: Performance Baseline — Design Spec

## Purpose

Establish reproducible performance baselines for the spectral visualization pipeline. These baselines serve as the reference point for future optimization work — no optimization happens in this phase.

## Scope

8 benchmarks covering the full pipeline:

1. **FFT** — raw FFT performance across sizes
2. **STFT** — full spectral analysis (window + FFT + magnitude)
3. **SpectralDataset** — binary serialization/deserialization
4. **Image Rendering** — SpectrogramRenderer pixel generation
5. **PNG Encoding** — image file writing
6. **Audio Conversion** — mono mixdown from stereo
7. **Media Decode** — WAV file decode via ffmpeg
8. **Video Rendering** — frame rendering pipeline

## What We Measure

| Metric | How | Why |
|--------|-----|-----|
| Wall time | `std::chrono::high_resolution_clock` | User-facing speed |
| Throughput | bytes processed / wall time | Scaling behavior |
| Peak memory | `GetProcessMemoryInfo` (Win), `/proc/self/status` (Linux) | Resource requirements |
| CPU time | `GetProcessTimes` (Win), `clock_gettime` (Linux) | Compute efficiency |

## What We Don't Measure Yet

- GPU utilization (no GPU code paths exist)
- Cache behavior (needs perf counters, not portable)
- Power consumption (needs hardware counters)
- Thread utilization (single-threaded pipeline)

## Design Decisions

1. **Synthetic data over real files**: All benchmarks generate data programmatically. Eliminates file I/O as a variable, makes results reproducible, avoids needing test assets.

2. **Median of 3 runs**: More robust than single run, less noisy than mean. Warmup run excluded.

3. **`--quick` flag**: Development mode runs only 1-minute tests. Full mode runs all durations.

4. **JSON output**: Machine-readable, diffable across commits. No external parsing dependencies.

5. **Single executable**: All benchmarks in one binary. Easier to build, deploy, and compare.

## Parameter Rationale

- **Sample rates**: 44.1kHz (CD), 48kHz (professional), 96kHz (high-res)
- **FFT sizes**: 2048 (fast), 4096 (standard), 8192 (high-res), 16384, 32768 (ultra)
- **Durations**: 1min (quick), 10min (standard), 1hr (stress test)
- **Resolutions**: 1024×512 (web), 1920×1080 (HD), 3840×2160 (4K)

## Files

See plan file for complete file list and implementation details.
