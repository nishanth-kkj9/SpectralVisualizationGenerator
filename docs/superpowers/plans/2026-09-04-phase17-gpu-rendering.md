# Phase 17 — GPU Spectrogram Rendering (DX11 Compute)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move spectrogram rendering to GPU via DirectX 11 Compute Shaders. Visually identical output. CPU fallback preserved.

**Architecture:** Two HLSL compute shaders (sample+normalize, colormap). D3D11 device for offscreen compute. Structured buffers for I/O. Staging buffer for readback.

**Tech Stack:** C++20, DirectX 11, HLSL, D3DCompiler.dll (runtime shader compilation)

**Spec:** `docs/superpowers/specs/2026-09-04-phase17-gpu-rendering-design.md`

## Global Constraints

- C++20, MSVC, CMake multi-config
- No external dependencies (D3D11 + D3DCompiler are Windows SDK)
- CPU fallback always available
- Build: `cmake --build build --config Release`
- Test: `ctest --test-dir build/`

---

### Task 1: D3D11 GPU Context

**Files:**
- Create: `src/core/rendering/d3d11_context.h`
- Create: `src/core/rendering/d3d11_context.cpp`

**Interfaces:**
- Produces: `D3D11Context` class with `is_available()`, device/context accessors, buffer creation helpers

- [ ] **Step 1: Create header**

```cpp
// src/core/rendering/d3d11_context.h
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <string>

namespace Spectral {

class D3D11Context {
public:
    D3D11Context();
    ~D3D11Context();

    bool is_available() const { return device_ != nullptr; }
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }

    // Buffer helpers
    Microsoft::WRL::ComPtr<ID3D11Buffer> create_structured_buffer(
        const void* data, uint32_t byte_width, uint32_t element_size,
        D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
        uint32_t bind_flags = D3D11_BIND_SHADER_RESOURCE);

    Microsoft::WRL::ComPtr<ID3D11Buffer> create_staging_buffer(uint32_t byte_width);

    bool upload_buffer(ID3D11Buffer* dst, const void* data, uint32_t byte_width);
    bool readback_buffer(ID3D11Buffer* src, void* out, uint32_t byte_width);

    // Shader compilation
    Microsoft::WRL::ComPtr<ID3DBlob> compile_shader(
        const std::string& hlsl_source,
        const std::string& entry_point,
        const std::string& target);

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> create_compute_shader(ID3DBlob* blob);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
};

} // namespace Spectral
```

- [ ] **Step 2: Create implementation**

Implement D3D11Context constructor (D3D11_CREATE_DEVICE_FLAG), buffer helpers (create_structured_buffer, create_staging_buffer, upload_buffer, readback_buffer), compile_shader (D3DCompileFromFile or D3DCompile), create_compute_shader.

- [ ] **Step 3: Build to verify compilation**

Run: `cmake --build build --config Release 2>&1 | Select-String -Pattern "error|d3d11_context"`

---

### Task 2: HLSL Compute Shaders

**Files:**
- Create: `src/core/rendering/shaders/spectrogram_cs.hlsl`
- Create: `src/core/rendering/shaders/colormap_cs.hlsl`

**Interfaces:**
- Consumes: magnitude buffer (StructuredBuffer<float>), config (push constants)
- Produces: normalized float buffer (RWStructuredBuffer<float>)

- [ ] **Step 1: Write spectrogram compute shader**

HLSL compute shader: each thread computes one output pixel. Frequency mapping via push constants (freq_min_unit, freq_max_unit, db_floor, db_ceiling). Bilinear interpolation between adjacent bins/frames. Output: normalized [0,1] float per pixel.

- [ ] **Step 2: Write colormap compute shader**

HLSL compute shader: each thread maps one normalized value to RGBA via 256-entry LUT (StructuredBuffer<float3>). Output: RGBA8 packed as uint per pixel.

- [ ] **Step 3: Build to verify shader files are copied**

---

### Task 3: GPU Spectrogram Renderer

**Files:**
- Create: `src/core/rendering/gpu_spectrogram.h`
- Create: `src/core/rendering/gpu_spectrogram.cpp`

**Interfaces:**
- Consumes: `D3D11Context`, `SpectralDataset`, `SpectrogramConfig`
- Produces: `RGBAImage` (same struct as CPU renderer)

- [ ] **Step 1: Create header**

```cpp
// src/core/rendering/gpu_spectrogram.h
#pragma once
#include "spectrogram_renderer.h"
#include "d3d11_context.h"
#include <memory>

namespace Spectral {

class GpuSpectrogram {
public:
    explicit GpuSpectrogram(D3D11Context& ctx);
    ~GpuSpectrogram();

    bool is_available() const;
    RenderError render(const SpectralDataset& dataset, RGBAImage& out);

private:
    D3D11Context& ctx_;
    // Cached shaders, buffers
};

} // namespace Spectral
```

- [ ] **Step 2: Implement render pipeline**

Steps: validate input → upload magnitude data as StructuredBuffer → upload LUT as StructuredBuffer → dispatch spectrogram_cs (ceil(width/16) × ceil(height/16) × 1) → dispatch colormap_cs (ceil(width*height/256) × 1 × 1) → readback staging buffer → fill RGBAImage.

- [ ] **Step 3: Build to verify**

---

### Task 4: CMake Integration

**Files:**
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `gpu_spectrogram` object lib, linked to d3d11.lib d3dcompiler.lib

- [ ] **Step 1: Add D3D11 to CMake**

Add d3d11.lib and d3dcompiler.lib to link libs. Add gpu_spectrogram.cpp to rendering sources. Add NOMINMAX define.

- [ ] **Step 2: Build full project**

Run: `cmake --build build --config Release`

---

### Task 5: Wire into SpectrogramRenderer

**Files:**
- Modify: `src/core/rendering/spectrogram_renderer.h` — add `render_gpu()` method
- Modify: `src/core/rendering/spectrogram_renderer.cpp` — implement `render_gpu()` delegating to GpuSpectrogram

- [ ] **Step 1: Add render_gpu() to SpectrogramRenderer**

```cpp
// In spectrogram_renderer.h, add:
RenderError render_gpu(const SpectralDataset& dataset, RGBAImage& out);

// In spectrogram_renderer.cpp, implement:
RenderError SpectrogramRenderer::render_gpu(const SpectralDataset& dataset, RGBAImage& out) {
    static D3D11Context ctx;
    static GpuSpectrogram gpu(ctx);
    if (!gpu.is_available()) return render(dataset, out); // CPU fallback
    return gpu.render(dataset, out);
}
```

- [ ] **Step 2: Build**

---

### Task 6: Correctness Tests

**Files:**
- Create: `tests/accuracy/test_gpu_render.cpp`

**Interfaces:**
- Consumes: SpectrogramRenderer (both render() and render_gpu()), SpectralDataset
- Produces: PSNR/SSIM comparison, pass/fail

- [ ] **Step 1: Write test**

Generate 440Hz sine dataset. Render CPU and GPU. Compute PSNR (must be > 40dB). Test all 4 interp×colormap combos.

- [ ] **Step 2: Add test to CMake**

- [ ] **Step 3: Build and run tests**

Run: `cmake --build build --config Release && ctest --test-dir build/ -R test_gpu_render -V`

---

### Task 7: CPU vs GPU Benchmark

**Files:**
- Create: `src/benchmarks/bench_gpu_render.cpp`

- [ ] **Step 1: Write benchmark**

Matrix: 3 resolutions × 2 color maps × 2 interpolation. Time CPU and GPU paths. Print table.

- [ ] **Step 2: Wire into main.cpp**

- [ ] **Step 3: Build and run benchmark**

Run: `cmake --build build --config Release && spectral_benchmarks.exe gpu_render`
