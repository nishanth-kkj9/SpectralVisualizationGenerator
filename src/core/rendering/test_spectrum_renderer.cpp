// Phase 7 — SpectrumRenderer tests
// - linear/log frequency
// - amplitude / dB
// - frequency limits
// - resolution
// - labels & grid
// - determinism
// - source dataset unchanged
// - golden fixtures

#include "spectrum_renderer.h"
#include "spectrogram_renderer.h"
#include "png_encoder.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Spectral;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                           \
    do {                                                                       \
        if (cond) { ++g_pass; }                                                \
        else {                                                                 \
            ++g_fail;                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);\
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                       \
        auto _a = (a); auto _b = (b);                                          \
        if (_a == _b) { ++g_pass; }                                            \
        else {                                                                 \
            ++g_fail;                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s == %s  (got %lld vs %lld)\n",  \
                __FILE__, __LINE__, #a, #b,                                   \
                (long long)_a, (long long)_b);                                 \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                 \
    do {                                                                       \
        double _a = (double)(a), _b = (double)(b), _t = (double)(tol);         \
        if (std::fabs(_a - _b) <= _t) { ++g_pass; }                            \
        else {                                                                 \
            ++g_fail;                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s ~= %s  (got %.6f vs %.6f)\n",  \
                __FILE__, __LINE__, #a, #b, _a, _b);                           \
        }                                                                      \
    } while (0)

// Build a dataset directly (no mutation of existing dataset).
// magnitudes[k] = amp for each k in peaks, 0 otherwise.
static SpectralDataset build_dataset(
    int n_fft, int sr,
    const std::vector<int>& peaks = {},
    const std::vector<float>& amps = {},
    int n_frames = 1)
{
    SpectralDataset d;
    d.mutable_analysis_metadata().fft_size = n_fft;
    d.mutable_analysis_metadata().hop_size = n_fft / 2;
    d.mutable_analysis_metadata().sample_rate = sr;
    d.mutable_analysis_metadata().nyquist_frequency = sr / 2.0f;
    d.mutable_analysis_metadata().num_frequency_bins = n_fft / 2 + 1;
    d.mutable_frequency_axis() = FrequencyAxis(n_fft, sr);
    d.mutable_time_axis() = TimeAxis(n_frames, n_fft / 2, sr);
    const int nb = n_fft / 2 + 1;
    for (int i = 0; i < n_frames; ++i) {
        Spectral::SpectralFrame f;
        f.n_fft = n_fft;
        f.frame_index = i;
        f.timestamp = i * (double)(n_fft / 2) / (double)sr;
        f.magnitudes.assign(nb, 0.0f);
        for (size_t p = 0; p < peaks.size(); ++p) {
            f.magnitudes[peaks[p]] = amps[p];
        }
        d.add_frame(f);
    }
    return d;
}

// Find the column whose topmost non-background pixel is highest in the image
// (i.e. the column that actually contains the spectrum line's peak value).
static int find_peak_column(const RGBAImage& img,
                            uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    int best_col = -1;
    int best_top = img.height;  // smaller row = higher
    for (int x = 0; x < img.width; ++x) {
        for (int y = 0; y < img.height; ++y) {
            const uint8_t* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
            if (!(p[0] == bg_r && p[1] == bg_g && p[2] == bg_b)) {
                if (y < best_top) { best_top = y; best_col = x; }
                break;  // first hit from top is enough
            }
        }
    }
    return best_col;
}

static void pixel(const RGBAImage& img, int x, int y,
                  uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint8_t* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    r = p[0]; g = p[1]; b = p[2];
}

static uint64_t checksum(const RGBAImage& img) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < img.pixels.size(); ++i) {
        h ^= img.pixels[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int ok(SpectrumError e) { return (int)e; }

// ----------------------------------------------------------------------------

static void test_empty_dataset() {
    std::fprintf(stderr, "[empty_dataset]\n");
    SpectralDataset d;
    SpectrumConfig cfg;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::EmptyDataset));
    CHECK_EQ(out.width, 0);
    CHECK_EQ(out.height, 0);
    CHECK_EQ((int)out.pixels.size(), 0);
}

static void test_invalid_dimensions() {
    std::fprintf(stderr, "[invalid_dimensions]\n");
    auto d = build_dataset(1024, 44100, {10}, {0.5f});
    SpectrumConfig cfg;
    cfg.width = 0;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::InvalidDimensions));

    cfg.width = 100; cfg.height = 0;
    r.set_config(cfg);
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::InvalidDimensions));
}

static void test_invalid_frequency_range() {
    std::fprintf(stderr, "[invalid_freq_range]\n");
    auto d = build_dataset(1024, 44100, {10}, {0.5f});
    SpectrumConfig cfg;
    cfg.freq_min_hz = 1000.0f;
    cfg.freq_max_hz = 500.0f;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::InvalidFrequencyRange));
}

static void test_basic_render_shape() {
    std::fprintf(stderr, "[basic_render_shape]\n");
    auto d = build_dataset(1024, 44100, {50}, {0.5f});
    SpectrumConfig cfg;
    cfg.width = 256; cfg.height = 128;
    cfg.freq_min_hz = 1.0f; cfg.freq_max_hz = 22050.0f;
    cfg.draw_labels = false; cfg.draw_grid = false;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));
    CHECK_EQ(out.width, 256);
    CHECK_EQ(out.height, 128);
    CHECK_EQ((int)out.pixels.size(), 256 * 128 * 4);
}

static void test_silence_renders_to_bottom() {
    std::fprintf(stderr, "[silence_bottom]\n");
    auto d = build_dataset(1024, 44100, {}, {}, 4);
    SpectrumConfig cfg;
    cfg.width = 200; cfg.height = 100;
    cfg.draw_labels = false; cfg.draw_grid = false;
    cfg.freq_min_hz = 1.0f; cfg.freq_max_hz = 22050.0f;
    cfg.y_scale = SpectrumScale::Decibels;
    cfg.db_floor = -60.0f; cfg.db_ceiling = 0.0f;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));
    int bottom_hits = 0;
    for (int x = 0; x < out.width; ++x) {
        for (int y = out.height - 3; y < out.height; ++y) {
            uint8_t r_, g_, b_;
            pixel(out, x, y, r_, g_, b_);
            if (!(r_ == cfg.bg_r && g_ == cfg.bg_g && b_ == cfg.bg_b)) ++bottom_hits;
        }
    }
    CHECK(bottom_hits > 0);
}

static void test_tone_peak_position_linear() {
    std::fprintf(stderr, "[tone_peak_linear]\n");
    auto d = build_dataset(1024, 1024, {100}, {1.0f}, 3);
    SpectrumConfig cfg;
    cfg.width = 1024; cfg.height = 256;
    cfg.draw_labels = false; cfg.draw_grid = false;
    cfg.freq_min_hz = 1.0f; cfg.freq_max_hz = 512.0f;
    cfg.freq_scale = FrequencyScale::Linear;
    cfg.y_scale = SpectrumScale::Decibels;
    cfg.db_floor = -60.0f; cfg.db_ceiling = 0.0f;
    cfg.aggregation = SpectrumAggregation::Mean;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));
    const int expected_col = static_cast<int>(std::lround(100.0f / 512.0f * (cfg.width - 1)));
    int peak = find_peak_column(out, cfg.bg_r, cfg.bg_g, cfg.bg_b);
    CHECK(std::abs(peak - expected_col) <= 2);
}

static void test_tone_peak_position_log() {
    std::fprintf(stderr, "[tone_peak_log]\n");
    auto d = build_dataset(1024, 48000, {10}, {1.0f}, 2);
    SpectrumConfig cfg;
    cfg.width = 1024; cfg.height = 256;
    cfg.draw_labels = false; cfg.draw_grid = false;
    cfg.freq_min_hz = 20.0f; cfg.freq_max_hz = 24000.0f;
    cfg.freq_scale = FrequencyScale::Logarithmic;
    cfg.y_scale = SpectrumScale::Decibels;
    cfg.db_floor = -60.0f; cfg.db_ceiling = 0.0f;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));
    const double lf = std::log10(468.75);
    const double lfmin = std::log10(20.0);
    const double lfmax = std::log10(24000.0);
    const int expected_col = static_cast<int>(std::lround((lf - lfmin) / (lfmax - lfmin) * (cfg.width - 1)));
    int peak = find_peak_column(out, cfg.bg_r, cfg.bg_g, cfg.bg_b);
    CHECK(std::abs(peak - expected_col) <= 2);
}

static void test_dB_scaling() {
    std::fprintf(stderr, "[db_scaling]\n");
    auto d = build_dataset(1024, 44100, {10}, {0.5f});
    SpectrumConfig cfg;
    cfg.width = 200; cfg.height = 100;
    cfg.draw_labels = false; cfg.draw_grid = false;
    cfg.freq_min_hz = 1.0f; cfg.freq_max_hz = 22050.0f;
    cfg.freq_scale = FrequencyScale::Linear;
    cfg.y_scale = SpectrumScale::Decibels;
    cfg.db_floor = -60.0f; cfg.db_ceiling = 0.0f;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));
    int peak = find_peak_column(out, cfg.bg_r, cfg.bg_g, cfg.bg_b);
    CHECK(peak >= 0);
    int row_top = -1;
    for (int y = 0; y < out.height; ++y) {
        uint8_t r_, g_, b_;
        pixel(out, peak, y, r_, g_, b_);
        if (!(r_ == cfg.bg_r && g_ == cfg.bg_g && b_ == cfg.bg_b)) { row_top = y; break; }
    }
    CHECK(row_top >= 0);
    const int expected_row = static_cast<int>(std::lround((6.02 / 60.0) * (cfg.height - 1)));
    CHECK(std::abs(row_top - expected_row) <= 2);
}

static void test_linear_scaling() {
    std::fprintf(stderr, "[linear_scaling]\n");
    auto d = build_dataset(1024, 44100, {10}, {0.5f});
    SpectrumConfig cfg;
    cfg.width = 200; cfg.height = 100;
    cfg.draw_labels = false; cfg.draw_grid = false;
    cfg.freq_min_hz = 1.0f; cfg.freq_max_hz = 22050.0f;
    cfg.y_scale = SpectrumScale::Linear;
    cfg.y_max_amplitude = 1.0f;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));
    int peak = find_peak_column(out, cfg.bg_r, cfg.bg_g, cfg.bg_b);
    CHECK(peak >= 0);
    int row_top = -1;
    for (int y = 0; y < out.height; ++y) {
        uint8_t r_, g_, b_;
        pixel(out, peak, y, r_, g_, b_);
        if (!(r_ == cfg.bg_r && g_ == cfg.bg_g && b_ == cfg.bg_b)) { row_top = y; break; }
    }
    CHECK(std::abs(row_top - (cfg.height - 1) / 2) <= 2);
}

static void test_freq_limits_clip() {
    std::fprintf(stderr, "[freq_limits_clip]\n");
    auto d = build_dataset(1024, 44100, {10}, {1.0f});
    SpectrumConfig cfg;
    cfg.width = 200; cfg.height = 100;
    cfg.draw_labels = false; cfg.draw_grid = false;
    cfg.freq_min_hz = 1000.0f; cfg.freq_max_hz = 5000.0f;
    cfg.freq_scale = FrequencyScale::Linear;
    cfg.y_scale = SpectrumScale::Decibels;
    cfg.db_floor = -60.0f; cfg.db_ceiling = 0.0f;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));
    int peak = find_peak_column(out, cfg.bg_r, cfg.bg_g, cfg.bg_b);
    CHECK(peak <= 1);
}

static void test_resolution_match() {
    std::fprintf(stderr, "[resolution_match]\n");
    // k_low  = 1, n=256, sr=44100 -> f = 44100/256 ≈ 172.27 Hz
    // k_high = 16, n=4096, sr=44100 -> f = 16*44100/4096 ≈ 172.27 Hz
    auto d_low  = build_dataset(256,  44100, {1},   {1.0f});
    auto d_high = build_dataset(4096, 44100, {16},  {1.0f});
    const float f_low  = 1.0f  * 44100.0f / 256.0f;
    const float f_high = 16.0f * 44100.0f / 4096.0f;
    CHECK_NEAR(f_low, f_high, 0.01);

    auto render_at = [&](const SpectralDataset& d) {
        SpectrumConfig cfg;
        cfg.width = 1024; cfg.height = 100;
        cfg.draw_labels = false; cfg.draw_grid = false;
        cfg.freq_min_hz = 1.0f; cfg.freq_max_hz = 22050.0f;
        cfg.freq_scale = FrequencyScale::Linear;
        cfg.y_scale = SpectrumScale::Decibels;
        cfg.db_floor = -60.0f; cfg.db_ceiling = 0.0f;
        SpectrumRenderer r(cfg);
        RGBAImage out;
        r.render(d, out);
        return find_peak_column(out, cfg.bg_r, cfg.bg_g, cfg.bg_b);
    };
    int c_low  = render_at(d_low);
    int c_high = render_at(d_high);
    CHECK(std::abs(c_low - c_high) <= 1);
}

static void test_aggregation_modes() {
    std::fprintf(stderr, "[aggregation_modes]\n");
    // Build a 4-frame dataset directly with varying amplitudes at bin 20.
    const int n_fft = 1024, sr = 44100;
    SpectralDataset d;
    d.mutable_analysis_metadata().fft_size = n_fft;
    d.mutable_analysis_metadata().hop_size = n_fft / 2;
    d.mutable_analysis_metadata().sample_rate = sr;
    d.mutable_analysis_metadata().nyquist_frequency = sr / 2.0f;
    d.mutable_analysis_metadata().num_frequency_bins = n_fft / 2 + 1;
    d.mutable_frequency_axis() = FrequencyAxis(n_fft, sr);
    d.mutable_time_axis() = TimeAxis(4, n_fft / 2, sr);
    const int nb = n_fft / 2 + 1;
    const float frame_amps[4] = {0.2f, 0.8f, 0.8f, 0.8f};
    for (int i = 0; i < 4; ++i) {
        Spectral::SpectralFrame f;
        f.n_fft = n_fft;
        f.frame_index = i;
        f.timestamp = i * (n_fft / 2) / (double)sr;
        f.magnitudes.assign(nb, 0.0f);
        f.magnitudes[20] = frame_amps[i];
        d.add_frame(f);
    }

    auto spec = SpectrumRenderer::aggregate(d, SpectrumAggregation::Mean);
    auto specmax = SpectrumRenderer::aggregate(d, SpectrumAggregation::Max);
    auto specfirst = SpectrumRenderer::aggregate(d, SpectrumAggregation::FirstFrame);
    CHECK_NEAR(spec[20], 0.65f, 1e-3);
    CHECK_NEAR(specmax[20], 0.8f, 1e-6);
    CHECK_NEAR(specfirst[20], 0.2f, 1e-6);
}

static void test_does_not_mutate_dataset() {
    std::fprintf(stderr, "[does_not_mutate_dataset]\n");
    auto d = build_dataset(1024, 44100, {50}, {0.7f}, 3);
    std::vector<uint8_t> before;
    CHECK(d.serialize_binary(before));

    SpectrumConfig cfg;
    cfg.width = 256; cfg.height = 128;
    cfg.freq_min_hz = 1.0f; cfg.freq_max_hz = 22050.0f;
    cfg.draw_labels = true; cfg.draw_grid = true;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));

    RGBAImage out2;
    r.render(d, out2);
    CHECK_EQ(checksum(out), checksum(out2));

    std::vector<uint8_t> after;
    CHECK(d.serialize_binary(after));
    CHECK(before == after);
    CHECK_EQ(d.frame_count(), 3);
    CHECK_NEAR(d.frame(0).magnitudes[50], 0.7f, 1e-6);
}

static void test_determinism() {
    std::fprintf(stderr, "[determinism]\n");
    auto d = build_dataset(2048, 48000,
        {10, 100, 500},
        {0.3f, 0.6f, 0.9f}, 8);
    SpectrumConfig cfg;
    cfg.width = 800; cfg.height = 400;
    cfg.draw_labels = true; cfg.draw_grid = true;
    cfg.freq_min_hz = 20.0f; cfg.freq_max_hz = 24000.0f;
    cfg.freq_scale = FrequencyScale::Logarithmic;
    cfg.y_scale = SpectrumScale::Decibels;
    cfg.db_floor = -80.0f; cfg.db_ceiling = 0.0f;
    cfg.color_map = ColorMap::Viridis;
    SpectrumRenderer r(cfg);
    RGBAImage a, b;
    CHECK_EQ(ok(r.render(d, a)), ok(SpectrumError::Ok));
    CHECK_EQ(ok(r.render(d, b)), ok(SpectrumError::Ok));
    CHECK_EQ(checksum(a), checksum(b));
}

static void test_determinism_across_instances() {
    std::fprintf(stderr, "[determinism_across_instances]\n");
    auto d = build_dataset(1024, 44100, {50}, {0.6f}, 4);
    SpectrumConfig cfg;
    cfg.width = 200; cfg.height = 100;
    cfg.freq_min_hz = 1.0f; cfg.freq_max_hz = 22050.0f;
    SpectrumRenderer r1(cfg), r2(cfg);
    RGBAImage a, b;
    r1.render(d, a); r2.render(d, b);
    CHECK_EQ(checksum(a), checksum(b));
}

static void test_color_maps() {
    std::fprintf(stderr, "[color_maps]\n");
    auto d = build_dataset(1024, 44100, {50}, {1.0f});
    SpectrumConfig cfg;
    cfg.width = 200; cfg.height = 100;
    cfg.draw_labels = false; cfg.draw_grid = false;
    cfg.freq_min_hz = 1.0f; cfg.freq_max_hz = 22050.0f;
    cfg.y_scale = SpectrumScale::Decibels;
    cfg.db_floor = -60.0f; cfg.db_ceiling = 0.0f;

    cfg.color_map = ColorMap::Viridis;
    SpectrumRenderer r1(cfg);
    RGBAImage v; r1.render(d, v);
    cfg.color_map = ColorMap::Heat;
    SpectrumRenderer r2(cfg);
    RGBAImage h; r2.render(d, h);
    CHECK(checksum(v) != checksum(h));
    int peak = find_peak_column(v, cfg.bg_r, cfg.bg_g, cfg.bg_b);
    CHECK(peak >= 0);
}

static void test_grid_and_labels() {
    std::fprintf(stderr, "[grid_and_labels]\n");
    auto d = build_dataset(1024, 44100, {100}, {0.5f});
    SpectrumConfig cfg;
    cfg.width = 400; cfg.height = 200;
    cfg.draw_labels = true; cfg.draw_grid = true;
    cfg.grid_divisions_x = 4; cfg.grid_divisions_y = 3;
    cfg.freq_min_hz = 20.0f; cfg.freq_max_hz = 20000.0f;
    cfg.freq_scale = FrequencyScale::Logarithmic;
    cfg.y_scale = SpectrumScale::Decibels;
    cfg.db_floor = -80.0f; cfg.db_ceiling = 0.0f;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));
    bool found_grid = false;
    for (int y = cfg.grid_divisions_y - 1; y < out.height; y += 5) {
        uint8_t r_, g_, b_;
        pixel(out, 0, y, r_, g_, b_);
        if (r_ == cfg.grid_r && g_ == cfg.grid_g && b_ == cfg.grid_b) { found_grid = true; break; }
    }
    CHECK(found_grid);
}

static void test_png_roundtrip() {
    std::fprintf(stderr, "[png_roundtrip]\n");
    auto d = build_dataset(1024, 44100, {50}, {0.5f});
    SpectrumConfig cfg;
    cfg.width = 128; cfg.height = 64;
    cfg.draw_labels = false; cfg.draw_grid = false;
    cfg.freq_min_hz = 1.0f; cfg.freq_max_hz = 22050.0f;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));

    const std::string path = "tests/phase7/_roundtrip.png";
    fs::create_directories("tests/phase7");
    CHECK(PNGEncoder::write_rgba(path, out.width, out.height, out.pixels.data()));

    int w, h;
    std::vector<uint8_t> px;
    CHECK(PNGEncoder::read_rgba(path, w, h, px));
    CHECK_EQ(w, out.width);
    CHECK_EQ(h, out.height);
    CHECK_EQ((int)px.size(), (int)out.pixels.size());
    uint64_t px_h = 1469598103934665603ull;
    for (auto v : px) { px_h ^= v; px_h *= 1099511628211ull; }
    CHECK_EQ(px_h, checksum(out));
}

static void test_golden_fixtures() {
    std::fprintf(stderr, "[golden_fixtures]\n");
    fs::create_directories("tests/phase7");

    struct Case { const char* name; FrequencyScale fs; ColorMap cm; };
    Case cases[] = {
        {"log_viridis", FrequencyScale::Logarithmic, ColorMap::Viridis},
        {"lin_viridis", FrequencyScale::Linear,      ColorMap::Viridis},
        {"log_heat",    FrequencyScale::Logarithmic, ColorMap::Heat},
        {"lin_heat",    FrequencyScale::Linear,      ColorMap::Heat},
    };
    auto d = build_dataset(2048, 48000,
        {10, 100, 500, 1000},
        {0.25f, 0.5f, 0.75f, 1.0f}, 8);
    for (const auto& c : cases) {
        SpectrumConfig cfg;
        cfg.width = 800; cfg.height = 400;
        cfg.draw_labels = true; cfg.draw_grid = true;
        cfg.freq_min_hz = 20.0f; cfg.freq_max_hz = 24000.0f;
        cfg.freq_scale = c.fs;
        cfg.y_scale = SpectrumScale::Decibels;
        cfg.db_floor = -80.0f; cfg.db_ceiling = 0.0f;
        cfg.color_map = c.cm;
        SpectrumRenderer r(cfg);
        RGBAImage out;
        CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));

        const std::string path = std::string("tests/phase7/golden_") + c.name + ".png";
        CHECK(PNGEncoder::write_rgba(path, out.width, out.height, out.pixels.data()));

        const std::string expected = path + ".expected.png";
        if (fs::exists(expected)) {
            int w, h;
            std::vector<uint8_t> px;
            if (PNGEncoder::read_rgba(expected, w, h, px)) {
                CHECK_EQ(w, out.width);
                CHECK_EQ(h, out.height);
                RGBAImage ref;
                ref.width = w; ref.height = h; ref.pixels = std::move(px);
                CHECK_EQ(checksum(ref), checksum(out));
            } else {
                std::fprintf(stderr, "warn: failed to read %s\n", expected.c_str());
            }
        }
    }
}

static void test_auto_freq_range() {
    std::fprintf(stderr, "[auto_freq_range]\n");
    auto d = build_dataset(1024, 48000, {100}, {0.5f});
    SpectrumConfig cfg;
    cfg.width = 400; cfg.height = 200;
    cfg.draw_labels = false; cfg.draw_grid = false;
    cfg.freq_min_hz = 1.0f;
    cfg.freq_max_hz = 0.0f;
    cfg.freq_scale = FrequencyScale::Linear;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));
    const int expected = static_cast<int>(std::lround(4687.5f / 24000.0f * (cfg.width - 1)));
    int peak = find_peak_column(out, cfg.bg_r, cfg.bg_g, cfg.bg_b);
    CHECK(std::abs(peak - expected) <= 2);
}

static void test_nan_inf_magnitudes_handled() {
    std::fprintf(stderr, "[nan_inf_handled]\n");
    const int n_fft = 1024, sr = 44100;
    SpectralDataset d;
    d.mutable_analysis_metadata().fft_size = n_fft;
    d.mutable_analysis_metadata().hop_size = n_fft / 2;
    d.mutable_analysis_metadata().sample_rate = sr;
    d.mutable_analysis_metadata().nyquist_frequency = sr / 2.0f;
    d.mutable_analysis_metadata().num_frequency_bins = n_fft / 2 + 1;
    d.mutable_frequency_axis() = FrequencyAxis(n_fft, sr);
    d.mutable_time_axis() = TimeAxis(1, n_fft / 2, sr);
    Spectral::SpectralFrame f;
    f.n_fft = n_fft;
    f.magnitudes.assign(n_fft / 2 + 1, 0.0f);
    f.magnitudes[20] = std::numeric_limits<float>::infinity();
    f.magnitudes[40] = std::numeric_limits<float>::quiet_NaN();
    f.magnitudes[60] = -1.0f;
    d.add_frame(f);

    SpectrumConfig cfg;
    cfg.width = 200; cfg.height = 100;
    cfg.draw_labels = false; cfg.draw_grid = false;
    cfg.freq_min_hz = 1.0f; cfg.freq_max_hz = 22050.0f;
    cfg.y_scale = SpectrumScale::Decibels;
    cfg.db_floor = -60.0f; cfg.db_ceiling = 0.0f;
    SpectrumRenderer r(cfg);
    RGBAImage out;
    CHECK_EQ(ok(r.render(d, out)), ok(SpectrumError::Ok));
    CHECK_EQ(out.width, 200);
    CHECK_EQ(out.height, 100);
    CHECK_EQ((int)out.pixels.size(), 200 * 100 * 4);
}

static void test_no_dsp_recompute() {
    std::fprintf(stderr, "[no_dsp_recompute]\n");
    auto d = build_dataset(1024, 44100, {10, 200}, {0.4f, 0.9f}, 16);
    auto s = SpectrumRenderer::aggregate(d, SpectrumAggregation::Mean);
    CHECK_NEAR(s[10], 0.4f, 1e-6);
    CHECK_NEAR(s[200], 0.9f, 1e-6);
    CHECK_NEAR(s[0], 0.0f, 1e-6);
}

static void test_render_to_png() {
    std::fprintf(stderr, "[render_to_png]\n");
    fs::create_directories("tests/phase7");
    auto d = build_dataset(1024, 44100, {50}, {0.5f});
    SpectrumConfig cfg;
    cfg.width = 256; cfg.height = 128;
    cfg.draw_labels = false; cfg.draw_grid = false;
    SpectrumRenderer r(cfg);
    CHECK_EQ(ok(r.render_to_png(d, "tests/phase7/_render_to_png.png")), ok(SpectrumError::Ok));
    CHECK(fs::exists("tests/phase7/_render_to_png.png"));
    std::error_code ec;
    CHECK(fs::file_size("tests/phase7/_render_to_png.png", ec) > 0);
}

int main() {
    test_empty_dataset();
    test_invalid_dimensions();
    test_invalid_frequency_range();
    test_basic_render_shape();
    test_silence_renders_to_bottom();
    test_tone_peak_position_linear();
    test_tone_peak_position_log();
    test_dB_scaling();
    test_linear_scaling();
    test_freq_limits_clip();
    test_resolution_match();
    test_aggregation_modes();
    test_does_not_mutate_dataset();
    test_determinism();
    test_determinism_across_instances();
    test_color_maps();
    test_grid_and_labels();
    test_png_roundtrip();
    test_golden_fixtures();
    test_auto_freq_range();
    test_nan_inf_magnitudes_handled();
    test_no_dsp_recompute();
    test_render_to_png();

    std::fprintf(stderr, "\n=== spectrum_renderer: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
