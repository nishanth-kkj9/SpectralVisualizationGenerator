# Phase 17 — GPU Spectrogram Rendering

**Date:** 2026-09-04
**Status:** Approved
**Depends on:** Phase 16 (SpectralBackend interface)

## Goal

Move spectrogram rendering to GPU via Vulkan compute shaders. Preserve visual output correctness. Keep CPU renderer as fallback. Measure upload/execute/readback costs separately.

## Scope

- **In:** SpectrogramRenderer GPU path (2D heatmap rendering)
- **Out:** SpectrumRenderer (1D line plot), GPU FFT, video output

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| GPU API | DirectX 11 compute (pivoted from Vulkan — no Vulkan SDK on machine; D3D11 headers/libs in Windows SDK, HLSL via D3DCompile, no installs) | Zero-dependency Windows path; Vulkan remains future option |
| Output tolerance | Visually identical (PSNR > 40dB, SSIM > 0.99) | Bit-exact parity fragile across IEEE-754 implementations |
| Shader count | 2 (sample + colorize) | Separates data-dependent sampling from color LUT swap |
| CPU fallback | Parallel class, config flag | Zero risk to existing rendering path |
| Buffer pooling | Yes, across frames | Amortizes upload cost for repeated renders |

## Architecture

### GPU Pipeline

```
SpectralDataset
  │
  ├─[upload: magnitude buffers + metadata SSBOs]
  │
  ▼
Compute Shader 1: spectrogram.comp
  │  Per-pixel: frequency mapping, bilinear interpolation, dB normalization
  │  Input:  magnitudes[], freq_axis, time_axis, config (ssbo)
  │  Output: normalized_values[] (float per pixel)
  │
  ▼
Compute Shader 2: colormap.comp
  │  Per-pixel: LUT lookup
  │  Input:  normalized_values[], color_lut[] (uniform)
  │  Output: rgba_pixels[] (RGBA8 per pixel)
  │
  ├─[readback: staging buffer → CPU]
  │
  ▼
RGBAImage (identical layout to CPU output)
```

### Workgroup sizing

- Shader 1: 16×16 workgroups (256 threads). Each thread computes one output pixel.
- Shader 2: 256×1 workgroups (256 threads). Each thread processes one pixel.

Dispatch dimensions: `ceil(width/16) × ceil(height/16)` for Shader 1.

## Files

### New files

| File | Purpose | Est. LOC |
|------|---------|----------|
| `src/core/rendering/gpu_context.h` | Vulkan instance/device/queue wrapper | 80 |
| `src/core/rendering/gpu_context.cpp` | Vulkan init, teardown, buffer helpers | 250 |
| `src/core/rendering/gpu_spectrogram.h` | GPU spectrogram renderer class | 60 |
| `src/core/rendering/gpu_spectrogram.cpp` | Shader dispatch, upload, readback | 350 |
| `src/core/rendering/shaders/spectrogram.comp` | Compute shader 1: sample + normalize | 80 |
| `src/core/rendering/shaders/colormap.comp` | Compute shader 2: LUT lookup | 40 |
| `src/benchmarks/bench_gpu_render.cpp` | CPU vs GPU benchmark | 120 |
| `tests/accuracy/test_gpu_render.cpp` | Output comparison tests | 150 |

### Modified files

| File | Change |
|------|--------|
| `CMakeLists.txt` | `find_package(Vulkan)`, shader compilation, new targets |
| `src/core/rendering/spectrogram_renderer.h` | Add `render_gpu()` delegating to GpuSpectrogram |

## Data Transfer

### CPU→GPU

| Data | Size (1920×1080) | Buffer type |
|------|-------------------|-------------|
| Magnitudes | 1920×1080×4 = 8.3MB | SSBO (staging + device-local) |
| Freq axis | 1080×4 = 4.3KB | Uniform buffer |
| Time axis | 1920×8 = 15.3KB | Uniform buffer |
| Config | 64 bytes | Push constants |
| Color LUT | 256×3×4 = 3KB | Uniform buffer |

### GPU→CPU

| Data | Size (1920×1080) | Buffer type |
|------|-------------------|-------------|
| RGBA pixels | 1920×1080×4 = 8.3MB | Staging buffer (host-visible) |

### Upload cost estimate

- 8.3MB magnitude upload at PCIe 3.0 x16: ~200µs
- 8.3MB readback at PCIe 3.0 x16: ~200µs
- Total transfer: ~400µs for 1920×1080
- GPU shader execution: ~50-200µs (depending on interpolation mode)
- **Expected speedup at 1920×1080:** 2-5x over CPU (transfer overhead eats most gain at small sizes)

## Synchronization

```
CPU                    GPU
 │                      │
 ├─ vkQueueSubmit ──────▶
 │   (command buffer)    │
 │                      ├─ execute shader 1
 │                      ├─ execute shader 2
 │                      ├─ copy to staging
 │                      ├─ signal fence
 ◀──── fence signal ────┤
 │                      │
 ├─ read staging buffer │
 │                      │
```

Single queue, single submit per render. No semaphore chains needed.

## CPU Fallback

```cpp
class SpectrogramRenderer {
public:
    RGBAImage render(const SpectralDataset& data, const SpectrogramConfig& config);
    RGBAImage render_gpu(const SpectralDataset& data, const SpectrogramConfig& config);
};
```

- `render()` — existing CPU path, unchanged
- `render_gpu()` — delegates to `GpuSpectrogram` internally
- `GpuContext::is_available()` — returns false if Vulkan init fails
- Factory: `SpectrogramRenderer::create(gpu_preferred=true)` — falls back to CPU if GPU unavailable

## Benchmark

### Matrix

| Dimension | Values |
|-----------|--------|
| Resolution | 512×256, 1024×512, 1920×1080 |
| Color map | Viridis, Heat |
| Interpolation | Nearest, Bilinear |

### Metrics per run

- CPU wall time (ms)
- GPU wall time (ms) — includes upload + execute + readback
- GPU upload time (ms)
- GPU execute time (ms)
- GPU readback time (ms)
- Throughput (MPixels/s)
- Peak memory (MB)

### Expected results

| Resolution | CPU (ms) | GPU (ms) | Speedup |
|------------|----------|----------|---------|
| 512×256 | ~5 | ~3 | 1.7x |
| 1024×512 | ~20 | ~5 | 4x |
| 1920×1080 | ~80 | ~15 | 5.3x |

Small resolutions: GPU transfer overhead dominates. Large resolutions: GPU wins.

## Correctness Test

1. Generate `SpectralDataset` with known signal (440Hz sine, 48kHz, 1s)
2. Render with CPU `SpectrogramRenderer` → reference image
3. Render with GPU `render_gpu()` → test image
4. Compute PSNR: must be > 40dB
5. Compute SSIM: must be > 0.99
6. Test all 4 combinations: {Nearest, Bilinear} × {Viridis, Heat}

## Error Handling

| Condition | Behavior |
|-----------|----------|
| Vulkan init fails | `GpuContext::is_available()` returns false, CPU fallback |
| Validation error | Debug callback prints to stderr (debug builds only) |
| Shader compile fails | Throws `std::runtime_error` with log |
| Readback timeout (>5s) | Throws `std::runtime_error` |
| Out of GPU memory | Throws `std::runtime_error` with diagnostics |

## Testing

| Test | What it verifies |
|------|------------------|
| `test_gpu_render_basic` | GPU init, render, readback completes |
| `test_gpu_render_correctness` | PSNR/SSIM thresholds met |
| `test_gpu_render_configurations` | All 4 interp×colormap combos work |
| `test_gpu_render_fallback` | CPU fallback works when GPU unavailable |
| `bench_gpu_render` | Timing comparison vs CPU |

## Risks

| Risk | Mitigation |
|------|------------|
| Vulkan SDK not installed | `find_package(Vulkan)` with REQUIRED, clear error message |
| AMD driver quirks | Use only core Vulkan 1.0 features, no extensions |
| Float precision differences | PSNR threshold (40dB) allows minor differences |
| Small image regression | Benchmark shows GPU slower for small sizes — document, don't optimize away |

## Future Work (NOT in this phase)

- GPU FFT via VkFFT (Phase 16 gpu_backend.h integration)
- SpectrumRenderer GPU path
- Video frame output
- Multi-GPU support
