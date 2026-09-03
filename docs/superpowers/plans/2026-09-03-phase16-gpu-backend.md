# Phase 16: Optional GPU Spectral Backend — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add abstract SpectralBackend interface with CPUBackend implementation and GPUBackend stub, enabling future GPU acceleration without adding dependencies today.

**Architecture:** Abstract interface delegates to existing CPU FFT functions. GPUBackend is a stub. All existing code unchanged via default parameter values.

**Tech Stack:** C++20, existing FFT/thread pool code, no new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-03-phase16-gpu-backend-design.md`

## Global Constraints

- C++20, MSVC 19.51, CMake multi-config (`cmake --build build --config Release`)
- All LSP errors are spurious — real build succeeds via cmake
- `SpectralCore` is INTERFACE (header-only)
- Per-subdirectory include paths needed for src/core/dsp targets
- `NOMINMAX` required as compile definition for benchmark target
- `fft()` is `inline` in `fft.h`
- API gotchas: `AudioBuffer`, `AudioFrame`, `MediaDecoder` are global scope
- User said STOP after Phase 16 — no further phases

---

## File Structure

| File | Action | Purpose |
|------|--------|---------|
| `src/core/dsp/spectral_backend.h` | CREATE | Abstract interface + factory |
| `src/core/dsp/cpu_backend.h` | CREATE | CPUBackend (wraps existing FFT) |
| `src/core/dsp/gpu_backend.h` | CREATE | GPUBackend stub |
| `src/core/spectral/multiband_analyzer.h` | MODIFY | Add optional backend param |
| `src/core/spectral/multiband_analyzer.cpp` | MODIFY | Use backend for FFT calls |
| `tests/accuracy/test_phase16.cpp` | CREATE | Backend tests |
| `src/benchmarks/bench_backend.cpp` | CREATE | Backend overhead benchmark |
| `CMakeLists.txt` | MODIFY | Add test + benchmark targets |

---

### Task 1: SpectralBackend Abstract Interface

**Files:**
- Create: `src/core/dsp/spectral_backend.h`
- Test: `tests/accuracy/test_phase16.cpp`

**Interfaces:**
- Produces: `SpectralBackend` abstract class with `name()`, `is_available()`, `fft()`, `fft_magnitude_power()`, `fft_batch()`

- [ ] **Step 1: Create spectral_backend.h with abstract interface**

```cpp
#pragma once

// Phase 16 — Abstract spectral backend interface.
// Provides a uniform API for FFT operations across CPU and (future) GPU backends.

#include <complex>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

using complex_f = std::complex<float>;

namespace Spectral {

class SpectralBackend {
public:
    virtual ~SpectralBackend() = default;

    // Backend identification
    virtual const char* name() const = 0;
    virtual bool is_available() const = 0;

    // Forward/inverse FFT (in-place, complex)
    virtual void fft(std::vector<complex_f>& x, bool inverse = false) const = 0;

    // Combined magnitude + power in single pass (avoids redundant sqrt)
    virtual std::pair<std::vector<float>, std::vector<float>>
        fft_magnitude_power(const std::vector<complex_f>& X) const = 0;

    // Batched real-to-complex FFT for STFT
    // input:  real samples [batch_size * n_fft]
    // output: complex results [batch_size * (n_fft/2+1)]
    virtual void fft_batch(const float* input, complex_f* output,
                           int n_fft, int batch_size) const = 0;

    // Factory: returns best available backend (CPU for now)
    static std::unique_ptr<SpectralBackend> create_default();
};

} // namespace Spectral
```

- [ ] **Step 2: Write test for interface existence**

Create `tests/accuracy/test_phase16.cpp`:

```cpp
#include "spectral_backend.h"
#include "cpu_backend.h"
#include <cassert>
#include <cstdio>
#include <vector>

static void test_backend_factory() {
    auto backend = Spectral::SpectralBackend::create_default();
    assert(backend != nullptr);
    assert(backend->is_available() == true);
    printf("  PASS test_backend_factory\n");
}

static void test_backend_name() {
    auto backend = Spectral::SpectralBackend::create_default();
    assert(std::string(backend->name()) == "cpu");
    printf("  PASS test_backend_name\n");
}

int main() {
    printf("=== Phase 16 Backend Tests ===\n");
    test_backend_factory();
    test_backend_name();
    printf("All Phase 16 backend tests passed.\n");
    return 0;
}
```

- [ ] **Step 3: Build to verify compilation**

Run: `cmake --build build --config Release 2>&1 | tail -5`
Expected: Build succeeds (test won't link yet — `create_default()` not implemented)

- [ ] **Step 4: Commit**

```bash
git add src/core/dsp/spectral_backend.h tests/accuracy/test_phase16.cpp
git commit -m "feat(phase16): add SpectralBackend abstract interface"
```

---

### Task 2: CPUBackend Implementation

**Files:**
- Create: `src/core/dsp/cpu_backend.h`
- Modify: `src/core/dsp/spectral_backend.h` (add `#include "cpu_backend.h"` in factory)

**Interfaces:**
- Consumes: `fft()` from `fft.h`, `fft_magnitude_power()` from `fft.h`, `ThreadPool` from `thread_pool.h`
- Produces: `CPUBackend` class, `SpectralBackend::create_default()` implementation

- [ ] **Step 1: Create cpu_backend.h**

```cpp
#pragma once

// Phase 16 — CPU backend for SpectralBackend interface.
// Wraps existing FFT functions with zero algorithmic changes.

#include "spectral_backend.h"
#include "fft.h"
#include "thread_pool.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace Spectral {

class CPUBackend : public SpectralBackend {
public:
    const char* name() const override { return "cpu"; }
    bool is_available() const override { return true; }

    void fft(std::vector<complex_f>& x, bool inverse = false) const override {
        ::fft(x, inverse);
    }

    std::pair<std::vector<float>, std::vector<float>>
    fft_magnitude_power(const std::vector<complex_f>& X) const override {
        return ::fft_magnitude_power(X);
    }

    void fft_batch(const float* input, complex_f* output,
                   int n_fft, int batch_size) const override {
        const int half = n_fft / 2 + 1;

        // ponytail: small batches run sequential, parallel threshold = 8
        if (batch_size < 8) {
            for (int b = 0; b < batch_size; ++b) {
                std::vector<complex_f> frame(n_fft);
                for (int i = 0; i < n_fft; ++i) {
                    frame[i] = complex_f(input[b * n_fft + i], 0.0f);
                }
                ::fft(frame, false);
                std::copy(frame.begin(), frame.begin() + half, output + b * half);
            }
            return;
        }

        // Parallel batch via thread pool
        ThreadPool pool;
        const int chunks = pool.size();
        const int chunk_size = (batch_size + chunks - 1) / chunks;

        for (int c = 0; c < chunks; ++c) {
            int start = c * chunk_size;
            int end = std::min(start + chunk_size, batch_size);
            pool.submit([=] {
                for (int b = start; b < end; ++b) {
                    std::vector<complex_f> frame(n_fft);
                    for (int i = 0; i < n_fft; ++i) {
                        frame[i] = complex_f(input[b * n_fft + i], 0.0f);
                    }
                    ::fft(frame, false);
                    std::copy(frame.begin(), frame.begin() + half, output + b * half);
                }
            });
        }
        // ThreadPool destructor joins all threads
    }
};

// Factory implementation
inline std::unique_ptr<SpectralBackend> SpectralBackend::create_default() {
    return std::make_unique<CPUBackend>();
}

} // namespace Spectral
```

- [ ] **Step 2: Write CPU backend tests**

Add to `tests/accuracy/test_phase16.cpp`:

```cpp
static void test_cpu_fft_matches_direct() {
    auto backend = Spectral::SpectralBackend::create_default();

    // Test vector: simple sinusoid
    int N = 1024;
    std::vector<complex_f> x1(N), x2(N);
    for (int i = 0; i < N; ++i) {
        float val = std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);
        x1[i] = complex_f(val, 0.0f);
        x2[i] = complex_f(val, 0.0f);
    }

    // Direct call
    ::fft(x1, false);
    // Backend call
    backend->fft(x2, false);

    // Compare (within float epsilon)
    for (int i = 0; i < N; ++i) {
        float diff = std::abs(x1[i] - x2[i]);
        assert(diff < 1e-4f);
    }
    printf("  PASS test_cpu_fft_matches_direct\n");
}

static void test_cpu_magnitude_power_matches() {
    auto backend = Spectral::SpectralBackend::create_default();

    int N = 512;
    std::vector<complex_f> x(N);
    for (int i = 0; i < N; ++i) {
        x[i] = complex_f(static_cast<float>(i) / N, 0.5f);
    }

    auto [mag1, pow1] = ::fft_magnitude_power(x);
    auto [mag2, pow2] = backend->fft_magnitude_power(x);

    for (int i = 0; i < N; ++i) {
        assert(std::abs(mag1[i] - mag2[i]) < 1e-5f);
        assert(std::abs(pow1[i] - pow2[i]) < 1e-5f);
    }
    printf("  PASS test_cpu_magnitude_power_matches\n");
}

static void test_cpu_fft_batch_matches_sequential() {
    auto backend = Spectral::SpectralBackend::create_default();

    int n_fft = 256;
    int batch = 4;
    std::vector<float> input(batch * n_fft);
    for (int i = 0; i < batch * n_fft; ++i) {
        input[i] = static_cast<float>(i) / (batch * n_fft);
    }

    // Batch result
    int half = n_fft / 2 + 1;
    std::vector<complex_f> batch_out(batch * half);
    backend->fft_batch(input.data(), batch_out.data(), n_fft, batch);

    // Sequential reference
    for (int b = 0; b < batch; ++b) {
        std::vector<complex_f> frame(n_fft);
        for (int i = 0; i < n_fft; ++i) {
            frame[i] = complex_f(input[b * n_fft + i], 0.0f);
        }
        ::fft(frame, false);
        for (int i = 0; i < half; ++i) {
            float diff = std::abs(frame[i] - batch_out[b * half + i]);
            assert(diff < 1e-4f);
        }
    }
    printf("  PASS test_cpu_fft_batch_matches_sequential\n");
}
```

- [ ] **Step 3: Update main() to run all tests**

Replace `main()` in test_phase16.cpp:

```cpp
int main() {
    printf("=== Phase 16 Backend Tests ===\n");
    test_backend_factory();
    test_backend_name();
    test_cpu_fft_matches_direct();
    test_cpu_magnitude_power_matches();
    test_cpu_fft_batch_matches_sequential();
    printf("All Phase 16 backend tests passed.\n");
    return 0;
}
```

- [ ] **Step 4: Build and run tests**

Run: `cmake --build build --config Release --target spectral_tests 2>&1 | tail -3`
Run: `build\Release\spectral_tests.exe 2>&1`
Expected: All tests PASS

- [ ] **Step 5: Commit**

```bash
git add src/core/dsp/cpu_backend.h src/core/dsp/spectral_backend.h tests/accuracy/test_phase16.cpp
git commit -m "feat(phase16): add CPUBackend implementation with tests"
```

---

### Task 3: GPUBackend Stub

**Files:**
- Create: `src/core/dsp/gpu_backend.h`

**Interfaces:**
- Consumes: `SpectralBackend` interface
- Produces: `GPUBackend` stub class

- [ ] **Step 1: Create gpu_backend.h**

```cpp
#pragma once

// Phase 16 — GPU backend stub for SpectralBackend interface.
// All methods throw. is_available() returns false.
// Documented extension point for future VkFFT/Vulkan integration.
//
// To implement a real GPU backend:
// 1. Add VkFFT as dependency (vcpkg or submodule)
// 2. Implement Vulkan device init, buffer management, shader dispatch
// 3. Replace this stub with real implementations
// 4. Define SPECTRAL_ENABLE_GPU in CMake
// 5. Update create_default() to detect and prefer GPU

#include "spectral_backend.h"
#include <stdexcept>

namespace Spectral {

class GPUBackend : public SpectralBackend {
public:
    const char* name() const override { return "gpu"; }
    bool is_available() const override { return false; }

    void fft(std::vector<complex_f>& /*x*/, bool /*inverse*/ = false) const override {
        throw std::runtime_error("GPU backend not implemented");
    }

    std::pair<std::vector<float>, std::vector<float>>
    fft_magnitude_power(const std::vector<complex_f>& /*X*/) const override {
        throw std::runtime_error("GPU backend not implemented");
    }

    void fft_batch(const float* /*input*/, complex_f* /*output*/,
                   int /*n_fft*/, int /*batch_size*/) const override {
        throw std::runtime_error("GPU backend not implemented");
    }
};

} // namespace Spectral
```

- [ ] **Step 2: Write GPU backend test**

Add to `tests/accuracy/test_phase16.cpp`:

```cpp
static void test_gpu_not_available() {
    Spectral::GPUBackend gpu;
    assert(gpu.is_available() == false);
    assert(std::string(gpu.name()) == "gpu");

    bool threw = false;
    try {
        std::vector<complex_f> x(4);
        gpu.fft(x);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    printf("  PASS test_gpu_not_available\n");
}
```

Add `#include "gpu_backend.h"` to the test file's includes.

- [ ] **Step 3: Build and run tests**

Run: `cmake --build build --config Release --target spectral_tests 2>&1 | tail -3`
Run: `build\Release\spectral_tests.exe 2>&1`
Expected: All tests PASS

- [ ] **Step 4: Commit**

```bash
git add src/core/dsp/gpu_backend.h tests/accuracy/test_phase16.cpp
git commit -m "feat(phase16): add GPUBackend stub"
```

---

### Task 4: Integrate Backend into MultiBandAnalyzer

**Files:**
- Modify: `src/core/spectral/multiband_analyzer.h:23-35`
- Modify: `src/core/spectral/multiband_analyzer.cpp`

**Interfaces:**
- Consumes: `SpectralBackend` interface
- Produces: `analyze()` and `analyze_single()` accept optional `SpectralBackend&` param

- [ ] **Step 1: Add backend parameter to header**

In `multiband_analyzer.h`, add forward declaration and optional parameter:

```cpp
// Add after existing includes:
#include "spectral_backend.h"

// Add default parameter to analyze():
static AnalyzeResult analyze(
    const std::vector<float>& samples,
    int sample_rate,
    int n_fft = 1024,
    int hop_size = 256,
    const SpectralBackend* backend = nullptr
);

// Add default parameter to analyze_single():
static AnalyzeResult analyze_single(
    const std::vector<float>& samples,
    int sample_rate,
    int n_fft,
    int hop_size,
    const SpectralBackend* backend = nullptr
);
```

- [ ] **Step 2: Update implementation to use backend**

In `multiband_analyzer.cpp`:
- Add `#include "cpu_backend.h"` at top
- In `analyze_single()`: if `backend != nullptr`, use `backend->fft()` and `backend->fft_magnitude_power()` instead of direct `::fft()` and `::fft_magnitude_power()`
- In `analyze()`: pass `backend` through to `analyze_single()` calls

- [ ] **Step 3: Add backend integration test**

Add to `tests/accuracy/test_phase16.cpp`:

```cpp
static void test_analyzer_with_backend() {
    // Create a simple test signal
    int N = 4096;
    int sr = 44100;
    std::vector<float> samples(N);
    for (int i = 0; i < N; ++i) {
        samples[i] = std::sin(2.0f * 3.14159f * 440.0f * i / sr);
    }

    auto cpu = std::make_unique<Spectral::CPUBackend>();

    // Without backend (default = CPU)
    auto r1 = Spectral::MultiBandAnalyzer::analyze_single(samples, sr, 1024, 256);

    // With explicit CPU backend
    auto r2 = Spectral::MultiBandAnalyzer::analyze_single(samples, sr, 1024, 256, cpu.get());

    // Results should match
    assert(r1.dataset.frame_count() == r2.dataset.frame_count());
    printf("  PASS test_analyzer_with_backend\n");
}
```

Add `#include "multiband_analyzer.h"` to test file includes.

- [ ] **Step 4: Build and run tests**

Run: `cmake --build build --config Release --target spectral_tests 2>&1 | tail -3`
Run: `build\Release\spectral_tests.exe 2>&1`
Expected: All tests PASS

- [ ] **Step 5: Commit**

```bash
git add src/core/spectral/multiband_analyzer.h src/core/spectral/multiband_analyzer.cpp tests/accuracy/test_phase16.cpp
git commit -m "feat(phase16): integrate backend into MultiBandAnalyzer"
```

---

### Task 5: Backend Overhead Benchmark

**Files:**
- Create: `src/benchmarks/bench_backend.cpp`
- Modify: `CMakeLists.txt` (add to benchmark target)

**Interfaces:**
- Consumes: `SpectralBackend`, `CPUBackend`
- Produces: `bench_backend_overhead()` benchmark function

- [ ] **Step 1: Create bench_backend.cpp**

```cpp
#include "benchmark_harness.h"
#include "cpu_backend.h"
#include "fft.h"
#include <vector>
#include <complex>
#include <cmath>

bench::Result bench_backend_overhead(int sample_rate, int fft_size, int duration_sec) {
    auto backend = Spectral::SpectralBackend::create_default();

    // Prepare test data
    int N = fft_size;
    int num_frames = (sample_rate * duration_sec) / N;
    if (num_frames < 1) num_frames = 1;

    std::vector<float> input(N);
    for (int i = 0; i < N; ++i) {
        input[i] = std::sin(2.0f * 3.14159f * 440.0f * i / sample_rate);
    }

    // Benchmark 1: Direct function call
    bench::Timer t_direct;
    for (int f = 0; f < num_frames; ++f) {
        std::vector<complex_f> frame(N);
        for (int i = 0; i < N; ++i) frame[i] = complex_f(input[i], 0.0f);
        ::fft(frame, false);
    }
    double direct_ms = t_direct.elapsed_ms();

    // Benchmark 2: Via backend interface
    bench::Timer t_backend;
    for (int f = 0; f < num_frames; ++f) {
        std::vector<complex_f> frame(N);
        for (int i = 0; i < N; ++i) frame[i] = complex_f(input[i], 0.0f);
        backend->fft(frame, false);
    }
    double backend_ms = t_backend.elapsed_ms();

    // Benchmark 3: Via backend fft_batch
    int half = N / 2 + 1;
    std::vector<float> batch_input(num_frames * N);
    std::vector<complex_f> batch_output(num_frames * half);
    for (int i = 0; i < num_frames * N; ++i) {
        batch_input[i] = std::sin(2.0f * 3.14159f * 440.0f * i / sample_rate);
    }

    bench::Timer t_batch;
    backend->fft_batch(batch_input.data(), batch_output.data(), N, num_frames);
    double batch_ms = t_batch.elapsed_ms();

    bench::Result r;
    r.benchmark = "backend_overhead";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;
    r.wall_ms = backend_ms;
    r.throughput_mbs = (static_cast<double>(num_frames) * N * sizeof(float)) / (backend_ms / 1000.0) / (1024.0 * 1024.0);
    r.peak_memory_mb = 0;
    r.cpu_time_ms = backend_ms;
    r.iterations = num_frames;
    r.extra = "direct=" + std::to_string((int)direct_ms) + "ms batch=" + std::to_string((int)batch_ms) + "ms";

    printf("    direct=%.1fms backend=%.1fms batch=%.1fms overhead=%.1f%%\n",
           direct_ms, backend_ms, batch_ms,
           (backend_ms - direct_ms) / direct_ms * 100.0);

    return r;
}
```

- [ ] **Step 2: Declare in main.cpp and wire up**

In `src/benchmarks/main.cpp`, add forward declaration:
```cpp
bench::Result bench_backend_overhead(int sample_rate, int fft_size, int duration_sec);
```

Add to `run_if` section (after existing benchmarks):
```cpp
run_if("backend_overhead", [&]() { return bench_backend_overhead(sr, fs, dur); });
```

- [ ] **Step 3: Build and run benchmark**

Run: `cmake --build build --config Release --target spectral_benchmarks 2>&1 | tail -3`
Run: `build\Release\spectral_benchmarks.exe --quick --benchmark backend_overhead 2>&1`
Expected: Benchmark runs, reports overhead percentage

- [ ] **Step 4: Commit**

```bash
git add src/benchmarks/bench_backend.cpp src/benchmarks/main.cpp CMakeLists.txt
git commit -m "feat(phase16): add backend overhead benchmark"
```

---

### Task 6: CMake Integration & Final Build Verification

**Files:**
- Modify: `CMakeLists.txt`

**Interfaces:**
- None new — wiring only

- [ ] **Step 1: Update CMakeLists.txt**

Add test file to test target and benchmark file to benchmark target. Ensure `src/core/dsp` is in include paths for both targets.

- [ ] **Step 2: Full build verification**

Run: `cmake --build build --config Release 2>&1 | tail -5`
Expected: Clean build, no errors

- [ ] **Step 3: Run full test suite**

Run: `build\Release\spectral_tests.exe 2>&1`
Expected: All tests PASS (existing + Phase 16)

- [ ] **Step 4: Run full benchmark suite**

Run: `build\Release\spectral_benchmarks.exe --quick 2>&1`
Expected: All benchmarks complete

- [ ] **Step 5: Final commit**

```bash
git add CMakeLists.txt
git commit -m "feat(phase16): complete CMake integration for GPU backend interface"
```
