#include "spectral_backend.h"
#include "cpu_backend.h"
#include "gpu_backend.h"
#include "fft.h"
#include <cmath>
#include <cstdio>
#include <vector>

using complex_f = std::complex<float>;

static bool approx_eq(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) < eps;
}

static int test_factory() {
    auto backend = Spectral::SpectralBackend::create_default();
    if (!backend) { printf("FAIL: create_default returned null\n"); return 1; }
    if (backend->name() != std::string("cpu")) { printf("FAIL: expected cpu, got %s\n", backend->name()); return 1; }
    if (!backend->is_available()) { printf("FAIL: cpu should be available\n"); return 1; }
    printf("PASS: factory\n");
    return 0;
}

static int test_fft_matches_reference() {
    Spectral::CPUBackend backend;
    std::vector<complex_f> x = { {1,0}, {1,0}, {1,0}, {1,0} };
    auto ref = x;

    backend.fft(x);
    ::fft(ref);

    for (size_t i = 0; i < x.size(); ++i) {
        if (!approx_eq(x[i].real(), ref[i].real()) || !approx_eq(x[i].imag(), ref[i].imag())) {
            printf("FAIL: fft mismatch at %zu: got (%.6f,%.6f) ref (%.6f,%.6f)\n",
                   i, x[i].real(), x[i].imag(), ref[i].real(), ref[i].imag());
            return 1;
        }
    }
    printf("PASS: fft_matches_reference\n");
    return 0;
}

static int test_magnitude_power_matches_reference() {
    Spectral::CPUBackend backend;
    std::vector<complex_f> x = { {3,0}, {0,4}, {-1,0}, {0,-2} };
    auto ref = x;

    auto [mag, pwr] = backend.fft_magnitude_power(x);
    auto [ref_mag, ref_pwr] = ::fft_magnitude_power(ref);

    for (size_t i = 0; i < mag.size(); ++i) {
        if (!approx_eq(mag[i], ref_mag[i])) {
            printf("FAIL: magnitude mismatch at %zu\n", i);
            return 1;
        }
        if (!approx_eq(pwr[i], ref_pwr[i])) {
            printf("FAIL: power mismatch at %zu\n", i);
            return 1;
        }
    }
    printf("PASS: magnitude_power_matches_reference\n");
    return 0;
}

static int test_gpu_stub_unavailable() {
    Spectral::GPUBackend gpu;
    if (gpu.is_available()) { printf("FAIL: gpu should not be available\n"); return 1; }
    if (gpu.name() != std::string("gpu")) { printf("FAIL: gpu name\n"); return 1; }

    std::vector<complex_f> x = { {1,0} };
    try {
        gpu.fft(x);
        printf("FAIL: gpu.fft should throw\n");
        return 1;
    } catch (const std::runtime_error&) {}

    try {
        gpu.fft_magnitude_power(x);
        printf("FAIL: gpu.fft_magnitude_power should throw\n");
        return 1;
    } catch (const std::runtime_error&) {}

    printf("PASS: gpu_stub_unavailable\n");
    return 0;
}

static int test_fft_batch_matches_sequential() {
    Spectral::CPUBackend backend;
    int n_fft = 256;
    int batch = 4;

    std::vector<float> input(n_fft * batch);
    for (int i = 0; i < n_fft * batch; ++i) {
        input[i] = static_cast<float>(i) * 0.001f;
    }

    std::vector<complex_f> batch_out(batch * (n_fft / 2 + 1));
    backend.fft_batch(input.data(), batch_out.data(), n_fft, batch);

    // Verify first frame matches sequential fft
    std::vector<complex_f> frame(n_fft);
    for (int i = 0; i < n_fft; ++i) {
        frame[i] = complex_f(input[i], 0.0f);
    }
    ::fft(frame);

    for (int i = 0; i < n_fft / 2 + 1; ++i) {
        if (!approx_eq(batch_out[i].real(), frame[i].real(), 1e-3f) ||
            !approx_eq(batch_out[i].imag(), frame[i].imag(), 1e-3f)) {
            printf("FAIL: batch frame 0 mismatch at bin %d\n", i);
            return 1;
        }
    }
    printf("PASS: fft_batch_matches_sequential\n");
    return 0;
}

int main() {
    int fails = 0;
    fails += test_factory();
    fails += test_fft_matches_reference();
    fails += test_magnitude_power_matches_reference();
    fails += test_gpu_stub_unavailable();
    fails += test_fft_batch_matches_sequential();

    printf("\nPhase 16: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails;
}
