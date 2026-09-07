// tests/cli/test_config_validation.cpp
// S4-D1/D2: window names fail closed, hop sizes are bounded. All checks
// go through validate_config (config boundary, before any decode) plus
// run_job rejection with no output file produced.
#include "pipeline.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>

namespace fs = std::filesystem;

static int g_run = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++g_run;                                                             \
        if (cond) {                                                          \
            ++g_pass;                                                        \
        } else {                                                             \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);            \
        }                                                                    \
    } while (0)

static Spectral::GenerateConfig base_cfg() {
    Spectral::GenerateConfig cfg;
    cfg.input_path = "no_such_file.wav";  // never reached: validation first
    cfg.output_path = "cfgval_should_not_exist.png";
    return cfg;
}

static void test_valid_windows() {
    std::printf("[valid_windows]\n");
    for (const char* w : {"hann", "hamming", "blackman", "rectangular"}) {
        auto cfg = base_cfg();
        cfg.window = w;
        CHECK(Spectral::validate_config(cfg).empty(), "window accepted");
    }
}

static void test_invalid_windows() {
    std::printf("[invalid_windows]\n");
    for (const char* w : {"haming", "foo", "HANN", "", "Hann", " hann"}) {
        auto cfg = base_cfg();
        cfg.window = w;
        const std::string err = Spectral::validate_config(cfg);
        CHECK(!err.empty(), "invalid window rejected");
        CHECK(err.find("invalid window") != std::string::npos, "names the problem");
        CHECK(err.find("hann, hamming, blackman, rectangular") != std::string::npos,
              "lists supported set");
    }
}

static void test_hop_contract() {
    std::printf("[hop_contract]\n");
    {
        auto cfg = base_cfg();
        cfg.fft_size = 1024;
        cfg.hop_size = 0;
        CHECK(Spectral::validate_config(cfg).empty(), "hop 0 = default");
    }
    for (int hop : {1, 512, 1024}) {
        auto cfg = base_cfg();
        cfg.fft_size = 1024;
        cfg.hop_size = hop;
        CHECK(Spectral::validate_config(cfg).empty(), "explicit hop in range");
    }
    for (int hop : {-1, -100}) {
        auto cfg = base_cfg();
        cfg.fft_size = 1024;
        cfg.hop_size = hop;
        const std::string err = Spectral::validate_config(cfg);
        CHECK(!err.empty() && err.find("hop-size") != std::string::npos, "negative hop rejected");
    }
    {
        auto cfg = base_cfg();
        cfg.fft_size = 1024;
        cfg.hop_size = 1025;
        const std::string err = Spectral::validate_config(cfg);
        CHECK(!err.empty() && err.find("hop-size") != std::string::npos, "hop past fft rejected");
    }
    {
        // fft_size=2 normalizes hop 0 -> 1, never zero-step
        auto cfg = base_cfg();
        cfg.fft_size = 2;
        cfg.hop_size = 0;
        CHECK(Spectral::validate_config(cfg).empty(), "fft=2 hop=0 valid");
    }
    {
        auto cfg = base_cfg();
        cfg.fft_size = 1;
        CHECK(!Spectral::validate_config(cfg).empty(), "fft=1 rejected");
    }
}

static void test_rejected_before_decode() {
    std::printf("[rejected_before_decode]\n");
    // Missing input + bad window must report BadConfig (validation runs
    // before any decode), and no output file may appear.
    {
        auto cfg = base_cfg();
        cfg.window = "haming";
        cfg.output_path = "cfgval_no_window.png";
        const Spectral::Error err = Spectral::run_job(cfg);
        CHECK(err.code == Spectral::JobError::BadConfig, "bad window -> BadConfig");
        CHECK(!fs::exists(cfg.output_path), "no output file produced");
    }
    {
        auto cfg = base_cfg();
        cfg.fft_size = 1024;
        cfg.hop_size = -5;
        cfg.output_path = "cfgval_no_hop.png";
        const Spectral::Error err = Spectral::run_job(cfg);
        CHECK(err.code == Spectral::JobError::BadConfig, "bad hop -> BadConfig");
        CHECK(!fs::exists(cfg.output_path), "no output file produced");
    }
}

static void test_strict_floats_and_scales() {
    std::printf("[strict_floats_and_scales]\n");
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    // NaN / inf in any user float fails at validation, never in DSP.
    for (float bad : {qnan, inf, -inf}) {
        auto cfg = base_cfg();
        cfg.min_freq = bad;
        CHECK(!Spectral::validate_config(cfg).empty(), "non-finite min_freq rejected");
    }
    {
        auto cfg = base_cfg();
        cfg.db_range = qnan;
        CHECK(!Spectral::validate_config(cfg).empty(), "NaN db_range rejected");
    }
    {
        auto cfg = base_cfg();
        cfg.overlap = 1.0f;
        CHECK(!Spectral::validate_config(cfg).empty(), "overlap=1 rejected");
    }
    {
        auto cfg = base_cfg();
        cfg.overlap = -0.0f;  // -0.0 compares == 0: still in [0,1)
        CHECK(Spectral::validate_config(cfg).empty(), "overlap=-0 accepted");
    }
    // Exactly six scales; anything else fails closed.
    for (const char* s :
         {"linear", "log", "mel", "bark", "erb", "cqt"}) {
        auto cfg = base_cfg();
        cfg.freq_scale = s;
        CHECK(Spectral::validate_config(cfg).empty(), "known scale accepted");
    }
    {
        auto cfg = base_cfg();
        cfg.freq_scale = "MEL";
        CHECK(!Spectral::validate_config(cfg).empty(), "uppercase scale rejected");
    }
    // fps/crf/duration/codec bind only to video output.
    {
        auto cfg = base_cfg();
        cfg.output_format = "image";
        cfg.fps = 0;
        cfg.crf = 99;
        CHECK(Spectral::validate_config(cfg).empty(), "image ignores fps/crf");
    }
    {
        auto cfg = base_cfg();
        cfg.output_format = "video";
        cfg.fps = 0;
        CHECK(!Spectral::validate_config(cfg).empty(), "video fps=0 rejected");
    }
    {
        auto cfg = base_cfg();
        cfg.output_format = "video";
        cfg.video_codec.clear();
        CHECK(!Spectral::validate_config(cfg).empty(), "video empty codec rejected");
    }
    // Pixel-count cap: typo-scale resolutions fail before allocating.
    {
        auto cfg = base_cfg();
        cfg.width = 32768;
        cfg.height = 32768;
        CHECK(!Spectral::validate_config(cfg).empty(), "32768x32768 rejected");
    }
    {
        auto cfg = base_cfg();
        cfg.width = 8192;
        cfg.height = 32768;
        CHECK(Spectral::validate_config(cfg).empty(), "8192x32768 at cap accepted");
    }
}

int main() {
    test_valid_windows();
    test_invalid_windows();
    test_hop_contract();
    test_rejected_before_decode();
    test_strict_floats_and_scales();
    std::printf("\n=== config_validation: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
