// tests/phase13/test_multiband.cpp
// Numerical tests for multi-band STFT
#include "fft.h"
#include "multiband_analyzer.h"
#include <cstdio>
#include <cmath>
#include <vector>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

// Generate a multi-frequency signal
static std::vector<float> make_signal(int sr, int n_samples) {
    std::vector<float> sig(n_samples);
    for (int i = 0; i < n_samples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(sr);
        sig[i] = 0.5f * std::sin(2.0f * PI * 500.0f * t)   // 500 Hz (low band)
               + 0.3f * std::sin(2.0f * PI * 3000.0f * t)   // 3000 Hz (mid band)
               + 0.2f * std::sin(2.0f * PI * 10000.0f * t);  // 10000 Hz (high band)
    }
    return sig;
}

static void test_default_bands() {
    std::fprintf(stderr, "[test_default_bands]\n");
    // Nyquist = 22050 Hz (sr=44100)
    auto bands = Spectral::MultiBandAnalyzer::default_bands(22050.0f);
    CHECK(bands.size() >= 2, "at least 2 bands");
    CHECK(bands.size() <= 3, "at most 3 bands");
    // First band starts at 0
    CHECK(bands.front().freq_low == 0.0f, "first band starts at 0");
    // Last band ends at or near Nyquist
    CHECK(bands.back().freq_high >= 20000.0f, "last band ends near Nyquist");
}

static void test_output_sizes() {
    std::fprintf(stderr, "[test_output_sizes]\n");
    int sr = 44100;
    auto sig = make_signal(sr, sr);  // 1 second
    auto result = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    CHECK(result.dataset.frame_count() > 0, "has frames");
    CHECK(result.dataset.num_frequency_bins() > 0, "has frequency bins");
    CHECK(result.dataset.analysis_metadata().analysis_method == "multiband_stft",
          "analysis method is multiband_stft");
}

static void test_magnitudes_nonzero() {
    std::fprintf(stderr, "[test_magnitudes_nonzero]\n");
    int sr = 44100;
    auto sig = make_signal(sr, sr);
    auto result = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    // At least some magnitudes should be nonzero
    float sum = 0;
    for (int i = 0; i < result.dataset.frame_count(); ++i) {
        const auto& frame = result.dataset.frame(i);
        for (float m : frame.magnitudes) {
            sum += m;
        }
    }
    CHECK(sum > 0.0f, "magnitudes are nonzero");
}

static void test_band_count() {
    std::fprintf(stderr, "[test_band_count]\n");
    int sr = 44100;
    auto sig = make_signal(sr, sr);
    auto result = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    CHECK(result.dataset.analysis_metadata().band_count >= 2, "metadata band_count >= 2");
    CHECK(result.dataset.frame(0).band_count >= 2, "frame band_count >= 2");
}

static void test_time_resolution() {
    std::fprintf(stderr, "[test_time_resolution]\n");
    int sr = 44100;
    auto sig = make_signal(sr, sr);
    auto result = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    // Time resolution should be reasonable (hop/sr)
    float time_res = static_cast<float>(result.dataset.analysis_metadata().hop_size) /
                     static_cast<float>(sr);
    CHECK(time_res > 0.0f, "time resolution > 0");
    CHECK(time_res < 0.1f, "time resolution < 100ms");
}

static void test_single_band() {
    std::fprintf(stderr, "[test_single_band]\n");
    int sr = 44100;
    auto sig = make_signal(sr, sr);

    // Short window should have better time resolution than long window
    auto short_r = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 256, 64);
    auto long_r = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 4096, 1024);

    float short_time_res = static_cast<float>(short_r.dataset.analysis_metadata().hop_size) /
                           static_cast<float>(sr);
    float long_time_res = static_cast<float>(long_r.dataset.analysis_metadata().hop_size) /
                          static_cast<float>(sr);

    CHECK(short_time_res < long_time_res, "short window has better time resolution");
    // Long window should have better frequency resolution
    CHECK(long_r.dataset.frequency_axis().resolution <
           short_r.dataset.frequency_axis().resolution,
          "long window has better frequency resolution");
}

static void test_fixed_stft() {
    std::fprintf(stderr, "[test_fixed_stft]\n");
    int sr = 44100;
    auto sig = make_signal(sr, sr);
    auto result = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 1024, 256);

    CHECK(result.dataset.analysis_metadata().analysis_method == "stft",
          "analysis method is stft");
    CHECK(result.dataset.analysis_metadata().band_count == 1, "band_count == 1");
    CHECK(result.dataset.frame_count() > 0, "has frames");
}

int main() {
    test_default_bands();
    test_output_sizes();
    test_magnitudes_nonzero();
    test_band_count();
    test_time_resolution();
    test_single_band();
    test_fixed_stft();
    std::fprintf(stderr, "\n=== multiband: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
