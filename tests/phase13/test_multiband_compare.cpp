// tests/phase13/test_multiband_compare.cpp
// Comparison tests for multi-band STFT
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

static std::vector<float> make_test_signal(int sr, int n_samples) {
    std::vector<float> sig(n_samples);
    for (int i = 0; i < n_samples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(sr);
        sig[i] = std::sin(2.0f * PI * 1000.0f * t);
    }
    return sig;
}

static void test_compare_all_four_methods() {
    std::fprintf(stderr, "[test_compare_all_four_methods]\n");
    int sr = 44100;
    auto sig = make_test_signal(sr, sr);

    auto fixed = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 1024, 256);
    auto short_w = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 256, 64);
    auto long_w = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 4096, 1024);
    auto multi = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    // All should produce frames
    CHECK(fixed.dataset.frame_count() > 0, "fixed has frames");
    CHECK(short_w.dataset.frame_count() > 0, "short_w has frames");
    CHECK(long_w.dataset.frame_count() > 0, "long_w has frames");
    CHECK(multi.dataset.frame_count() > 0, "multi has frames");

    // Short window has better time resolution
    float short_hop = static_cast<float>(short_w.dataset.analysis_metadata().hop_size);
    float long_hop = static_cast<float>(long_w.dataset.analysis_metadata().hop_size);
    CHECK(short_hop < long_hop, "short window has smaller hop");

    // Long window has better frequency resolution
    CHECK(long_w.dataset.frequency_axis().resolution <
           short_w.dataset.frequency_axis().resolution,
          "long window has better freq resolution");
}

static void test_compare_multiband_metadata() {
    std::fprintf(stderr, "[test_compare_multiband_metadata]\n");
    int sr = 44100;
    auto sig = make_test_signal(sr, sr);
    auto result = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    CHECK(result.dataset.analysis_metadata().analysis_method == "multiband_stft",
          "method is multiband_stft");
    CHECK(result.dataset.analysis_metadata().band_count >= 2, "band_count >= 2");
}

static void test_compare_multiband_vs_fixed() {
    std::fprintf(stderr, "[test_compare_multiband_vs_fixed]\n");
    int sr = 44100;
    auto sig = make_test_signal(sr, sr);

    auto fixed = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 1024, 256);
    auto multi = Spectral::MultiBandAnalyzer::analyze(sig, sr, 1024, 256);

    // Both should produce output
    CHECK(fixed.dataset.frame_count() > 0, "fixed has frames");
    CHECK(multi.dataset.frame_count() > 0, "multi has frames");

    // Both should have the same sample rate
    CHECK(fixed.dataset.analysis_metadata().sample_rate ==
           multi.dataset.analysis_metadata().sample_rate,
          "same sample rate");
}

static void test_regression_standard_stft_unchanged() {
    std::fprintf(stderr, "[test_regression_standard_stft_unchanged]\n");
    // Standard STFT without --multiband should produce identical results
    int sr = 44100;
    auto sig = make_test_signal(sr, sr);

    auto result1 = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 1024, 256);
    auto result2 = Spectral::MultiBandAnalyzer::analyze_single(sig, sr, 1024, 256);

    // Same input should produce same output
    CHECK(result1.dataset.frame_count() == result2.dataset.frame_count(),
          "same frame count");
    CHECK(result1.dataset.analysis_metadata().analysis_method == "stft",
          "method is stft");
    CHECK(result1.dataset.analysis_metadata().band_count == 1,
          "band_count == 1");
}

int main() {
    test_compare_all_four_methods();
    test_compare_multiband_metadata();
    test_compare_multiband_vs_fixed();
    test_regression_standard_stft_unchanged();
    std::fprintf(stderr, "\n=== multiband_compare: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
