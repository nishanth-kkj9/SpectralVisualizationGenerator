// tests/rendering/test_freqscale_render.cpp
#include "spectrogram_renderer.h"
#include "spectrum_renderer.h"
#include "spectral_dataset.h"
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

static Spectral::SpectralDataset make_test_ds() {
    using namespace Spectral;
    SpectralDataset ds;
    const int sr = 44100, fft = 1024, hop = 512, nf = 80;
    ds.mutable_frequency_axis() = FrequencyAxis(fft, sr);
    for (int i = 0; i < nf; ++i) {
        Spectral::SpectralFrame f;
        f.frame_index = i;
        f.n_fft = fft;
        f.window_factor = 0.5f;
        f.timestamp = double(i * hop) / sr;
        f.magnitudes.resize(fft / 2 + 1, 0.0f);
        f.phases.resize(fft / 2 + 1, 0.0f);
        f.power.resize(fft / 2 + 1, 0.0f);
        for (int k = 0; k < fft / 2 + 1; ++k) {
            float d1 = std::abs(k - 19.0f);
            float d2 = std::abs(k - 93.0f);
            float mag = std::exp(-d1*d1/8.0f)*0.3f + std::exp(-d2*d2/8.0f)*0.2f;
            f.magnitudes[k] = mag;
            f.power[k] = mag * mag;
        }
        f.rms = 0.15f;
        f.peak_magnitude = 0.5f;
        f.spectral_centroid = 1000.0f;
        ds.add_frame(f);
    }
    ds.mutable_time_axis() = TimeAxis(nf, hop, sr);
    auto& am = ds.mutable_analysis_metadata();
    am.fft_size = fft; am.hop_size = hop; am.sample_rate = sr;
    return ds;
}

static void test_spectrogram_all_scales() {
    std::fprintf(stderr, "[test_spectrogram_all_scales]\n");
    auto ds = make_test_ds();
    const char* names[] = {"linear", "log", "mel", "bark", "erb", "cqt"};
    Spectral::FrequencyScale scales[] = {
        Spectral::FrequencyScale::Linear,
        Spectral::FrequencyScale::Logarithmic,
        Spectral::FrequencyScale::Mel,
        Spectral::FrequencyScale::Bark,
        Spectral::FrequencyScale::Erb,
        Spectral::FrequencyScale::CQT,
    };
    for (int i = 0; i < 6; ++i) {
        Spectral::SpectrogramConfig cfg;
        cfg.width = 256; cfg.height = 128;
        cfg.freq_scale = scales[i];
        cfg.freq_min_hz = 20.0f; cfg.freq_max_hz = 20000.0f;
        if (scales[i] == Spectral::FrequencyScale::CQT) {
            cfg.cqt_center_hz = 440.0f;
            cfg.cqt_q = 12.0f;
        }
        Spectral::SpectrogramRenderer rend(cfg);
        Spectral::RGBAImage img;
        auto err = rend.render(ds, img);
        CHECK(err == Spectral::RenderError::Ok,
              (std::string("spectrogram ") + names[i] + " renders").c_str());
        CHECK(img.width == 256 && img.height == 128,
              (std::string("spectrogram ") + names[i] + " dims").c_str());
        int nonzero = 0;
        for (size_t p = 0; p < img.pixels.size(); p += 4) {
            if (img.pixels[p] > 0 || img.pixels[p+1] > 0 || img.pixels[p+2] > 0)
                nonzero++;
        }
        CHECK(nonzero > 100,
              (std::string("spectrogram ") + names[i] + " has content").c_str());
    }
}

static void test_spectrum_all_scales() {
    std::fprintf(stderr, "[test_spectrum_all_scales]\n");
    auto ds = make_test_ds();
    const char* names[] = {"linear", "log", "mel", "bark", "erb", "cqt"};
    Spectral::FrequencyScale scales[] = {
        Spectral::FrequencyScale::Linear,
        Spectral::FrequencyScale::Logarithmic,
        Spectral::FrequencyScale::Mel,
        Spectral::FrequencyScale::Bark,
        Spectral::FrequencyScale::Erb,
        Spectral::FrequencyScale::CQT,
    };
    for (int i = 0; i < 6; ++i) {
        Spectral::SpectrumConfig cfg;
        cfg.width = 256; cfg.height = 128;
        cfg.freq_scale = scales[i];
        cfg.freq_min_hz = 20.0f; cfg.freq_max_hz = 20000.0f;
        if (scales[i] == Spectral::FrequencyScale::CQT) {
            cfg.cqt_center_hz = 440.0f;
            cfg.cqt_q = 12.0f;
        }
        Spectral::SpectrumRenderer rend(cfg);
        Spectral::RGBAImage img;
        auto err = rend.render(ds, img);
        CHECK(err == Spectral::SpectrumError::Ok,
              (std::string("spectrum ") + names[i] + " renders").c_str());
    }
}

int main() {
    test_spectrogram_all_scales();
    test_spectrum_all_scales();
    std::fprintf(stderr, "\n=== freqscale_render: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
