// tests/cli/test_config_validation.cpp
// S4-D1/D2: window names fail closed, hop sizes are bounded. All checks
// go through validate_config (config boundary, before any decode) plus
// run_job rejection with no output file produced.
#include "pipeline.h"

#include <cstdio>
#include <filesystem>
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

int main() {
    test_valid_windows();
    test_invalid_windows();
    test_hop_contract();
    test_rejected_before_decode();
    std::printf("\n=== config_validation: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
