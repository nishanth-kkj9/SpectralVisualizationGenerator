# Phase 16: Optional GPU Spectral Backend — Design Spec

## Overview

Add an abstract `SpectralBackend` interface with a CPU implementation and a
stub GPU extension point. GPU support is optional, CPU fallback always exists.

## Motivation

Phase 15 parallelized the spectrogram renderer with a thread pool (11-62x
speedup on image rendering). The CPU FFT runs at ~30-45 MB/s. Investigation
shows GPU FFT (VkFFT via Vulkan) would add significant dependency weight
(Vulkan SDK + glslang ~50MB) while likely being slower for this project's
typical workloads (small-to-medium FFT sizes, moderate batch counts) on the
target hardware (AMD Radeon integrated graphics, 2 CU).

The backend interface provides clean architectural separation and a genuine
extension point without adding dependencies that don't help today.

## Architecture

```
SpectralBackend (abstract interface)
 ├── CPUBackend        ← wraps existing cpu_fft + thread pool
 └── GPUBackend        ← stub, throws on use, documents extension path
```

## Interface

```cpp
class SpectralBackend {
public:
    virtual ~SpectralBackend() = default;
    virtual const char* name() const = 0;
    virtual bool is_available() const = 0;

    // Forward/inverse FFT (in-place, complex)
    virtual void fft(std::vector<complex_f>& x, bool inverse = false) const = 0;

    // Combined magnitude + power in single pass
    virtual std::pair<std::vector<float>, std::vector<float>>
        fft_magnitude_power(const std::vector<complex_f>& X) const = 0;

    // Batched real-to-complex FFT for STFT
    // input: interleaved real samples [batch_size * n_fft]
    // output: complex results [batch_size * (n_fft/2+1)]
    virtual void fft_batch(const float* input, complex_f* output,
                           int n_fft, int batch_size) const = 0;

    // Factory: returns best available backend
    static std::unique_ptr<SpectralBackend> create_default();
};
```

## CPUBackend

Header-only class in `cpu_backend.h`. Delegates to existing free functions:
- `fft()` from `fft.h`
- `fft_magnitude_power()` from `fft.h`
- `fft_batch()`: new static helper that calls `fft()` in a loop over batch
  (threaded via ThreadPool for large batches)

Zero algorithmic changes. Pure delegation.

## GPUBackend

Stub class in `gpu_backend.h`. Methods throw `std::runtime_error`.
`is_available()` returns false. Documented as extension point for future
VkFFT/Vulkan integration.

Gated behind `#ifdef SPECTRAL_ENABLE_GPU` in CMake (never defined by default).

## Files

| File | Action | Purpose |
|------|--------|---------|
| `src/core/dsp/spectral_backend.h` | NEW | Abstract interface + factory |
| `src/core/dsp/cpu_backend.h` | NEW | CPUBackend implementation |
| `src/core/dsp/gpu_backend.h` | NEW | GPUBackend stub |
| `src/core/spectral/multiband_analyzer.h` | MODIFY | Add optional `SpectralBackend&` param |
| `src/core/spectral/multiband_analyzer.cpp` | MODIFY | Use backend for FFT calls |
| `src/benchmarks/main.cpp` | MODIFY | Add backend comparison benchmark |

## Backward Compatibility

All existing code works unchanged. The backend is opt-in:
- `MultiBandAnalyzer::analyze()` and `analyze_single()` get a new optional
  parameter with default value
- Default = CPU backend = identical behavior to today
- No existing function signatures change (only gain optional parameter)

## Testing

1. `CPUBackend::fft()` produces bit-identical results to existing `fft()`
2. `CPUBackend::fft_magnitude_power()` matches existing implementation
3. `CPUBackend::fft_batch()` matches sequential loop of `fft()`
4. `create_default()` returns non-null backend
5. `CPUBackend::is_available()` returns true
6. `GPUBackend::is_available()` returns false
7. Virtual dispatch overhead < 1% of FFT time (measured via benchmark)

## Benchmark

Add `bench_backend_comparison()` to benchmark suite:
- Runs FFT workload through `SpectralBackend` interface vs direct function call
- Measures overhead of abstraction layer
- Reports: wall time, throughput, virtual dispatch overhead percentage

## What This Does NOT Do

- No Vulkan/CUDA/HIP/OpenCL dependencies
- No GPU compute code
- No new FFT algorithms
- No changes to the CPU reference implementation
- No performance regression (pure delegation, zero overhead path)

## Future Extension

When a user has a discrete GPU and workloads large enough to benefit:
1. Implement `VkGPUBackend` in `gpu_backend.h`
2. Add VkFFT as vcpkg/port or submodule
3. Define `SPECTRAL_ENABLE_GPU` in CMake
4. Update `create_default()` to detect and prefer GPU when available
5. Run benchmark comparison to verify GPU actually helps
