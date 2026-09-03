#pragma once

// Phase 16 — Abstract spectral backend interface.
// Provides a uniform API for FFT operations across CPU and (future) GPU backends.

#include <complex>
#include <memory>
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
