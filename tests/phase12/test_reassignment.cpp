// tests/phase12/test_reassignment.cpp
// Numerical tests for time-frequency reassignment
#include "fft.h"
#include "windows.h"
#include <cstdio>
#include <cmath>
#include <vector>

// PI from fft.h
#ifndef PI
#define PI 3.14159265358979323846f
#endif

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

// Helper: compute reassignment coordinates for a windowed frame
static void compute_reassignment(const float* samples, const float* window,
                                  int n_fft, int sr,
                                  std::vector<float>& out_freq,
                                  std::vector<float>& out_time) {
    const int half = n_fft / 2 + 1;
    const float threshold = 1e-12f;

    // Standard STFT
    std::vector<complex_f> X(n_fft);
    for (int i = 0; i < n_fft; ++i) {
        X[i] = complex_f(samples[i] * window[i], 0.0f);
    }
    fft(X);

    // Time-derivative STFT: FFT{n * w[n] * x[n]} (group delay kernel)
    std::vector<complex_f> X_tau(n_fft);
    for (int i = 0; i < n_fft; ++i) {
        X_tau[i] = complex_f(static_cast<float>(i) * window[i] * samples[i], 0.0f);
    }
    fft(X_tau);

    // Window derivative STFT: FFT{w'[n] * x[n]} (frequency reassignment kernel)
    std::vector<float> w_deriv(n_fft);
    for (int n = 0; n < n_fft; ++n) {
        w_deriv[n] = static_cast<float>(PI) / static_cast<float>(n_fft - 1) *
                     std::sin(2.0f * PI * static_cast<float>(n) / static_cast<float>(n_fft - 1));
    }
    std::vector<complex_f> X_dg(n_fft);
    for (int i = 0; i < n_fft; ++i) {
        X_dg[i] = complex_f(w_deriv[i] * samples[i], 0.0f);
    }
    fft(X_dg);

    out_freq.resize(half);
    out_time.resize(half);

    for (int k = 0; k < half; ++k) {
        float re = X[k].real();
        float im = X[k].imag();
        float mag_sq = re * re + im * im;

        if (mag_sq > threshold) {
            complex_f conj_X(re, -im);
            // Instantaneous frequency: base freq + correction from window derivative
            float corr_freq = (conj_X * X_dg[k]).imag();
            out_freq[k] = static_cast<float>(k) * static_cast<float>(sr) / static_cast<float>(n_fft) +
                          corr_freq / (2.0f * PI * mag_sq) * static_cast<float>(sr);
            // Group delay: center of mass in time
            float dot_tau = (conj_X * X_tau[k]).real();
            out_time[k] = dot_tau / mag_sq / static_cast<float>(sr);
        } else {
            out_freq[k] = 0.0f;
            out_time[k] = 0.0f;
        }
    }
}

// Test: impulse at center should have group delay near 0 (center of window)
static void test_impulse_group_delay() {
    std::fprintf(stderr, "[test_impulse_group_delay]\n");
    const int n_fft = 1024;
    const int sr = 44100;

    std::vector<float> window(n_fft);
    for (int n = 0; n < n_fft; ++n) {
        window[n] = 0.5f * (1.0f - std::cos(2.0f * PI * static_cast<float>(n) / static_cast<float>(n_fft - 1)));
    }

    // Impulse at center
    std::vector<float> samples(n_fft, 0.0f);
    samples[n_fft / 2] = 1.0f;

    std::vector<float> freq, time;
    compute_reassignment(samples.data(), window.data(), n_fft, sr, freq, time);

    // Group delay at center bin should be near window center (N/2 samples)
    float expected_delay = static_cast<float>(n_fft / 2) / static_cast<float>(sr);
    float center_time = time[n_fft / 4]; // bin near DC has clean group delay
    CHECK(std::abs(center_time - expected_delay) < 0.002f,
          "impulse group delay near window center");
}

// Test: pure tone should have reassigned frequency near true frequency
static void test_pure_tone_frequency() {
    std::fprintf(stderr, "[test_pure_tone_frequency]\n");
    const int n_fft = 1024;
    const int sr = 44100;
    const float freq = 1000.0f;

    std::vector<float> window(n_fft);
    for (int n = 0; n < n_fft; ++n) {
        window[n] = 0.5f * (1.0f - std::cos(2.0f * PI * static_cast<float>(n) / static_cast<float>(n_fft - 1)));
    }

    // Pure tone
    std::vector<float> samples(n_fft);
    for (int n = 0; n < n_fft; ++n) {
        samples[n] = std::sin(2.0f * PI * freq * static_cast<float>(n) / static_cast<float>(sr));
    }

    std::vector<float> rfreq, rtime;
    compute_reassignment(samples.data(), window.data(), n_fft, sr, rfreq, rtime);

    // Find bin with max magnitude
    // (we can use the FFT magnitude from the computation)
    std::vector<complex_f> X(n_fft);
    for (int i = 0; i < n_fft; ++i) {
        X[i] = complex_f(samples[i] * window[i], 0.0f);
    }
    fft(X);

    int max_bin = 0;
    float max_mag = 0.0f;
    for (int k = 1; k < n_fft / 2; ++k) {
        float m = X[k].real() * X[k].real() + X[k].imag() * X[k].imag();
        if (m > max_mag) {
            max_mag = m;
            max_bin = k;
        }
    }

    // Reassigned frequency should be near 1000 Hz
    CHECK(std::abs(rfreq[max_bin] - freq) < 50.0f,
          "reassigned frequency near 1000 Hz");
}

// Test: chirp should span the correct frequency range
static void test_chirp_range() {
    std::fprintf(stderr, "[test_chirp_range]\n");
    const int n_fft = 1024;
    const int sr = 44100;

    std::vector<float> window(n_fft);
    for (int n = 0; n < n_fft; ++n) {
        window[n] = 0.5f * (1.0f - std::cos(2.0f * PI * static_cast<float>(n) / static_cast<float>(n_fft - 1)));
    }

    // Chirp from 500 Hz to 2000 Hz
    std::vector<float> samples(n_fft);
    for (int n = 0; n < n_fft; ++n) {
        float t = static_cast<float>(n) / static_cast<float>(sr);
        float f = 500.0f + 1500.0f * t;
        samples[n] = std::sin(2.0f * PI * f * t);
    }

    std::vector<float> rfreq, rtime;
    compute_reassignment(samples.data(), window.data(), n_fft, sr, rfreq, rtime);

    // Reassigned frequencies should span 500-2000 Hz range
    float min_freq = 1e6f, max_freq = 0.0f;
    for (int k = 1; k < n_fft / 2; ++k) {
        if (std::abs(rfreq[k]) > 100.0f) {
            min_freq = (std::min)(min_freq, rfreq[k]);
            max_freq = (std::max)(max_freq, rfreq[k]);
        }
    }

    CHECK(min_freq < 600.0f, "chirp min freq near 500 Hz");
    CHECK(max_freq > 1900.0f, "chirp max freq near 2000 Hz");
}

// Test: reassigned coordinates have correct sizes
static void test_sizes() {
    std::fprintf(stderr, "[test_sizes]\n");
    const int n_fft = 512;
    const int sr = 44100;

    std::vector<float> window(n_fft);
    std::vector<float> samples(n_fft, 0.0f);

    std::vector<float> freq, time;
    compute_reassignment(samples.data(), window.data(), n_fft, sr, freq, time);

    CHECK(static_cast<int>(freq.size()) == n_fft / 2 + 1, "freq size = n_fft/2+1");
    CHECK(static_cast<int>(time.size()) == n_fft / 2 + 1, "time size = n_fft/2+1");
}

// Test: silent frame should produce zeros
static void test_silent_frame() {
    std::fprintf(stderr, "[test_silent_frame]\n");
    const int n_fft = 1024;
    const int sr = 44100;

    std::vector<float> window(n_fft);
    for (int n = 0; n < n_fft; ++n) {
        window[n] = 0.5f * (1.0f - std::cos(2.0f * PI * static_cast<float>(n) / static_cast<float>(n_fft - 1)));
    }

    std::vector<float> samples(n_fft, 0.0f);

    std::vector<float> freq, time;
    compute_reassignment(samples.data(), window.data(), n_fft, sr, freq, time);

    bool all_zero = true;
    for (int k = 0; k < n_fft / 2 + 1; ++k) {
        if (freq[k] != 0.0f || time[k] != 0.0f) {
            all_zero = false;
            break;
        }
    }
    CHECK(all_zero, "silent frame produces zeros");
}

int main() {
    test_impulse_group_delay();
    test_pure_tone_frequency();
    test_chirp_range();
    test_sizes();
    test_silent_frame();
    std::fprintf(stderr, "\n=== reassignment: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
