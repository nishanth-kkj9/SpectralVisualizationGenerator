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
