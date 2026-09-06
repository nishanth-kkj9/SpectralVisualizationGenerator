// tests/phase12/test_reassignment_render.cpp
// Regression tests comparing conventional vs reassigned spectrogram rendering
#include "spectrogram_renderer.h"
#include "spectrum_renderer.h"
#include "spectral_dataset.h"
#include "fft.h"
#include "windows.h"
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

static void compute_reassignment(const float* samples, const float* window,
                                  int n_fft, int sr,
                                  std::vector<float>& out_freq,
                                  std::vector<float>& out_time) {
    const int half = n_fft / 2 + 1;
    const float threshold = 1e-12f;

    std::vector<complex_f> X(static_cast<size_t>(n_fft));
    for (int i = 0; i < n_fft; ++i) {
        X[static_cast<size_t>(i)] = complex_f(samples[i] * window[i], 0.0f);
    }
    fft(X);

    std::vector<complex_f> X_t(static_cast<size_t>(n_fft));
    for (int i = 0; i < n_fft; ++i) {
        X_t[static_cast<size_t>(i)] = complex_f(
            static_cast<float>(i) * window[i] * samples[i], 0.0f);
    }
    fft(X_t);

    std::vector<complex_f> X_f(static_cast<size_t>(n_fft));
    for (int k = 0; k < n_fft; ++k) {
        X_f[static_cast<size_t>(k)] = complex_f(
            static_cast<float>(k) * X[static_cast<size_t>(k)].real(),
            static_cast<float>(k) * X[static_cast<size_t>(k)].imag());
    }

    out_freq.resize(half);
    out_time.resize(half);

    for (int k = 0; k < half; ++k) {
        float re = X[static_cast<size_t>(k)].real();
        float im = X[static_cast<size_t>(k)].imag();
        float mag_sq = re * re + im * im;
        if (mag_sq > threshold) {
            complex_f conj_X(re, -im);
            float dot_t = (conj_X * X_t[static_cast<size_t>(k)]).imag();
            out_freq[static_cast<size_t>(k)] =
                dot_t / (2.0f * PI * mag_sq) *
                static_cast<float>(sr) / static_cast<float>(n_fft);
            float dot_f = (conj_X * X_f[static_cast<size_t>(k)]).real();
            out_time[static_cast<size_t>(k)] =
                dot_f / (2.0f * PI * mag_sq) / static_cast<float>(sr);
        } else {
            out_freq[static_cast<size_t>(k)] = 0.0f;
            out_time[static_cast<size_t>(k)] = 0.0f;
        }
    }
}

static Spectral::SpectralDataset make_test_ds(bool reassigned) {
    using namespace Spectral;
    SpectralDataset ds;
    const int sr = 44100, fft_size = 1024, hop = 512, nf = 20;
    ds.mutable_frequency_axis() = FrequencyAxis(fft_size, sr);

    std::vector<float> win(fft_size);
    for (int n = 0; n < fft_size; ++n) {
        win[n] = 0.5f * (1.0f - std::cos(2.0f * PI * static_cast<float>(n) /
                         static_cast<float>(fft_size - 1)));
    }

    for (int i = 0; i < nf; ++i) {
        std::vector<float> samples(fft_size);
        float freq = 500.0f + 1500.0f * static_cast<float>(i) / static_cast<float>(nf);
        for (int n = 0; n < fft_size; ++n) {
            float t = static_cast<float>(n) / static_cast<float>(sr);
            samples[n] = std::sin(2.0f * PI * freq * t);
        }

        // Compute STFT
        std::vector<complex_f> buf(fft_size);
        for (int n = 0; n < fft_size; ++n) {
            buf[n] = complex_f(samples[n] * win[n], 0.0f);
        }
        fft(buf);

        Spectral::SpectralFrame f;
        f.frame_index = i;
        f.n_fft = fft_size;
        f.timestamp = static_cast<double>(i * hop) / static_cast<double>(sr);
        f.magnitudes.resize(fft_size / 2 + 1);
        f.phases.resize(fft_size / 2 + 1);
        f.power.resize(fft_size / 2 + 1);

        float scale = 1.0f / static_cast<float>(fft_size);
        for (int k = 0; k < fft_size / 2 + 1; ++k) {
            float re = buf[k].real();
            float im = buf[k].imag();
            float mag = std::sqrt(re * re + im * im) * scale;
            f.magnitudes[k] = mag;
            f.phases[k] = std::atan2(im, re);
            f.power[k] = mag * mag;
        }

        if (reassigned) {
            compute_reassignment(samples.data(), win.data(), fft_size, sr,
                                f.reassigned_freqs, f.reassigned_times);
        }

        ds.add_frame(f);
    }

    ds.mutable_time_axis() = TimeAxis(nf, hop, sr);
    auto& am = ds.mutable_analysis_metadata();
    am.fft_size = fft_size; am.hop_size = hop; am.sample_rate = sr;
    return ds;
}

static void test_conventional_vs_reassigned() {
    std::fprintf(stderr, "[test_conventional_vs_reassigned]\n");

    auto ds_conv = make_test_ds(false);
    auto ds_reas = make_test_ds(true);

    CHECK(ds_conv.frame_count() == ds_reas.frame_count(), "same frame count");
    CHECK(ds_conv.num_frequency_bins() == ds_reas.num_frequency_bins(), "same freq bins");
    CHECK(!ds_reas.frame(0).reassigned_freqs.empty(), "reassigned has freq coords");
    CHECK(!ds_reas.frame(0).reassigned_times.empty(), "reassigned has time coords");
    CHECK(ds_conv.frame(0).reassigned_freqs.empty(), "conventional has no freq coords");
    CHECK(ds_conv.frame(0).reassigned_times.empty(), "conventional has no time coords");
}

static void test_reassigned_spectrogram_render() {
    std::fprintf(stderr, "[test_reassigned_spectrogram_render]\n");

    auto ds = make_test_ds(true);

    Spectral::SpectrogramConfig cfg;
    cfg.width = 256; cfg.height = 128;
    cfg.freq_scale = Spectral::FrequencyScale::Linear;
    cfg.freq_min_hz = 20.0f; cfg.freq_max_hz = 20000.0f;

    Spectral::SpectrogramRenderer rend(cfg);
    Spectral::RGBAImage img;
    auto err = rend.render(ds, img);
    CHECK(err == Spectral::RenderError::Ok, "reassigned spectrogram renders");
    CHECK(img.width == 256 && img.height == 128, "reassigned spectrogram dims");

    int nonzero = 0;
    for (size_t p = 0; p < img.pixels.size(); p += 4) {
        if (img.pixels[p] > 0 || img.pixels[p+1] > 0 || img.pixels[p+2] > 0)
            nonzero++;
    }
    CHECK(nonzero > 100, "reassigned spectrogram has content");
}

static void test_conventional_still_works() {
    std::fprintf(stderr, "[test_conventional_still_works]\n");

    auto ds = make_test_ds(false);

    Spectral::SpectrogramConfig cfg;
    cfg.width = 256; cfg.height = 128;
    cfg.freq_scale = Spectral::FrequencyScale::Linear;
    cfg.freq_min_hz = 20.0f; cfg.freq_max_hz = 20000.0f;

    Spectral::SpectrogramRenderer rend(cfg);
    Spectral::RGBAImage img;
    auto err = rend.render(ds, img);
    CHECK(err == Spectral::RenderError::Ok, "conventional spectrogram renders");

    int nonzero = 0;
    for (size_t p = 0; p < img.pixels.size(); p += 4) {
        if (img.pixels[p] > 0 || img.pixels[p+1] > 0 || img.pixels[p+2] > 0)
            nonzero++;
    }
    CHECK(nonzero > 100, "conventional spectrogram has content");
}

int main() {
    test_conventional_vs_reassigned();
    test_reassigned_spectrogram_render();
    test_conventional_still_works();
    std::fprintf(stderr, "\n=== reassignment_render: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
