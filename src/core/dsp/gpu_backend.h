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
