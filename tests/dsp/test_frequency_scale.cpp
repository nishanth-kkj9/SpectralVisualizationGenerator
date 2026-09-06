// tests/dsp/test_frequency_scale.cpp
#include "frequency_scale.h"
#include <cstdio>
#include <cmath>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

static void test_mel_conversion() {
    std::fprintf(stderr, "[test_mel_conversion]\n");
    CHECK(std::abs(Spectral::hz_to_mel(0.0f)) < 0.01f, "0 Hz = 0 mel");
    CHECK(std::abs(Spectral::hz_to_mel(1000.0f) - 1000.0f) < 1.0f, "1000 Hz ~= 1000 mel");
    for (float hz : {100.0f, 440.0f, 1000.0f, 4000.0f, 8000.0f, 16000.0f}) {
        float mel = Spectral::hz_to_mel(hz);
        float back = Spectral::mel_to_hz(mel);
        CHECK(std::abs(back - hz) / hz < 0.01f, "mel roundtrip");
    }
}

static void test_bark_conversion() {
    std::fprintf(stderr, "[test_bark_conversion]\n");
    CHECK(std::abs(Spectral::hz_to_bark(0.0f)) < 0.01f, "0 Hz = 0 bark");
    float b1k = Spectral::hz_to_bark(1000.0f);
    CHECK(b1k > 8.0f && b1k < 9.5f, "1000 Hz ~= 8.7 bark");
    for (float hz : {100.0f, 440.0f, 1000.0f, 4000.0f, 8000.0f}) {
        float bark = Spectral::hz_to_bark(hz);
        float back = Spectral::bark_to_hz(bark);
        CHECK(std::abs(back - hz) / hz < 0.02f, "bark roundtrip");
    }
}

static void test_erb_conversion() {
    std::fprintf(stderr, "[test_erb_conversion]\n");
    CHECK(std::abs(Spectral::hz_to_erb(0.0f)) < 0.01f, "0 Hz = 0 ERB-rate");
    float e1k = Spectral::hz_to_erb(1000.0f);
    CHECK(e1k > 120.0f && e1k < 140.0f, "1000 Hz ~= 132 ERB-rate");
    for (float hz : {50.0f, 200.0f, 1000.0f, 6000.0f, 12000.0f}) {
        float erb = Spectral::hz_to_erb(hz);
        float back = Spectral::erb_to_hz(erb);
        CHECK(std::abs(back - hz) / hz < 0.01f, "erb roundtrip");
    }
}

static void test_cqt_bin() {
    std::fprintf(stderr, "[test_cqt_bin]\n");
    CHECK(std::abs(Spectral::hz_to_cqt_bin(440.0f, 440.0f, 12.0f)) < 0.01f,
          "center bin = 0");
    CHECK(Spectral::hz_to_cqt_bin(880.0f, 440.0f, 12.0f) > 0.0f, "octave up > 0");
    float b = Spectral::hz_to_cqt_bin(880.0f, 440.0f, 12.0f);
    CHECK(std::abs(b - 12.0f) < 0.1f, "octave = 12 bins at Q=12");
}

static void test_hz_to_unit_scales() {
    std::fprintf(stderr, "[test_hz_to_unit_scales]\n");
    float fmin = 20.0f, fmax = 20000.0f;
    CHECK(std::abs(Spectral::hz_to_unit(fmin, 0, fmin, fmax) - 0.0f) < 0.01f, "linear: fmin=0");
    CHECK(std::abs(Spectral::hz_to_unit(fmax, 0, fmin, fmax) - 1.0f) < 0.01f, "linear: fmax=1");
    CHECK(std::abs(Spectral::hz_to_unit(fmin, 1, fmin, fmax) - 0.0f) < 0.01f, "log: fmin=0");
    CHECK(std::abs(Spectral::hz_to_unit(fmax, 1, fmin, fmax) - 1.0f) < 0.01f, "log: fmax=1");
    CHECK(std::abs(Spectral::hz_to_unit(fmin, 2, fmin, fmax) - 0.0f) < 0.01f, "mel: fmin=0");
    CHECK(std::abs(Spectral::hz_to_unit(fmax, 2, fmin, fmax) - 1.0f) < 0.01f, "mel: fmax=1");
    CHECK(std::abs(Spectral::hz_to_unit(fmin, 3, fmin, fmax) - 0.0f) < 0.01f, "bark: fmin=0");
    CHECK(std::abs(Spectral::hz_to_unit(fmax, 3, fmin, fmax) - 1.0f) < 0.01f, "bark: fmax=1");
    CHECK(std::abs(Spectral::hz_to_unit(fmin, 4, fmin, fmax) - 0.0f) < 0.01f, "erb: fmin=0");
    CHECK(std::abs(Spectral::hz_to_unit(fmax, 4, fmin, fmax) - 1.0f) < 0.01f, "erb: fmax=1");
    float mid_lin = Spectral::hz_to_unit(1000.0f, 0, fmin, fmax);
    float mid_log = Spectral::hz_to_unit(1000.0f, 1, fmin, fmax);
    float mid_mel = Spectral::hz_to_unit(1000.0f, 2, fmin, fmax);
    CHECK(mid_lin > 0.0f && mid_lin < 1.0f, "linear mid in (0,1)");
    CHECK(mid_log > 0.0f && mid_log < 1.0f, "log mid in (0,1)");
    CHECK(mid_mel > 0.0f && mid_mel < 1.0f, "mel mid in (0,1)");
}

static void test_monotonicity() {
    std::fprintf(stderr, "[test_monotonicity]\n");
    float fmin = 20.0f, fmax = 20000.0f;
    for (int scale = 0; scale <= 4; ++scale) {
        float prev = -1.0f;
        for (float hz = 20.0f; hz <= 20000.0f; hz *= 1.5f) {
            float u = Spectral::hz_to_unit(hz, scale, fmin, fmax);
            if (prev >= 0.0f) CHECK(u >= prev - 0.001f, "monotonic");
            prev = u;
        }
    }
}

int main() {
    test_mel_conversion();
    test_bark_conversion();
    test_erb_conversion();
    test_cqt_bin();
    test_hz_to_unit_scales();
    test_monotonicity();
    std::fprintf(stderr, "\n=== frequency_scale: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
