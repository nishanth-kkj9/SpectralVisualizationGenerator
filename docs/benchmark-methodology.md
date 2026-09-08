# Benchmark Methodology (release summary)

Harness: `spectral_benchmarks` (`--quick` = 60 s durations), median of 3 runs
after warmup, wall time + peak RSS. Machine: AMD Ryzen 5 7520U, 15 GB RAM,
Windows 11, Release build. Rerun with
`spectral_benchmarks --quick --output results.json`.

Measured 2026-09-06 (quick mode, 60 s inputs):

| Benchmark | Result |
|-----------|--------|
| FFT | 28–40 MB/s wall |
| STFT | ~8–10 MB/s wall |
| Dataset serialization | ~300 MB/s |
| Image rendering (1024×512 / 1920×1080) | ~11–44 MB/s throughput |
| PNG encoding | ~107–139 MB/s |
| GPU vs CPU render, 512×256 | CPU 4.5–7.6 ms, GPU 3.5–3.8 ms |
| GPU vs CPU render, 1024×512 viridis | CPU ~14–16 ms, GPU ~7–9 ms |
| GPU vs CPU render, 1024×512 heat | CPU ~13–17 ms, GPU ~9–15 ms |

GPU wins grow with resolution; at small sizes transfer overhead dominates.
No performance numbers beyond these measured results are claimed.

Streaming analysis (Phase 6), measured 2026-09-09 with
`spectral_benchmarks --quick --benchmark streaming_analysis`
(60 s inputs, single iteration per cell, same machine class):

| Case | audio throughput | peak RSS | peak live raw-audio buffer |
|------|-----------------|----------|---------------------------|
| 44.1 kHz, fft 2048–8192 | ~10–13 MB/s | ~43–47 MB | 24–48 KiB |
| 96 kHz, fft 2048–8192 | ~12–15 MB/s | ~85 MB | 24–48 KiB |

Peak RSS grows with sample rate because the `SpectralDataset` holds more
spectral frames for the same duration; the raw-audio working set stays at
tens of KiB regardless of duration.
