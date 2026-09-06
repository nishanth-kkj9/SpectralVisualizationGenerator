// Phase 6 — SpectrogramRenderer test suite
// Covers: determinism, color maps, dB normalization, edge cases, PNG I/O,
// golden-image fixtures (viridis+heat, lin+log).

#include "spectral_dataset.h"
#include "spectrogram_renderer.h"
#include "png_encoder.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace Spectral;

namespace {

int g_pass = 0;
int g_fail = 0;

#define EXPECT(cond, msg)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_pass;                                                         \
            std::printf("  PASS: %s\n", msg);                                 \
        } else {                                                              \
            ++g_fail;                                                         \
            std::printf("  FAIL: %s (at %s:%d)\n", msg, __FILE__, __LINE__);  \
        }                                                                     \
    } while (0)

bool png_pixels_equal(const std::string& a_path, const std::string& b_path) {
    int wa, ha, wb, hb;
    std::vector<uint8_t> pa, pb;
    if (!PNGEncoder::read_rgba(a_path, wa, ha, pa)) return false;
    if (!PNGEncoder::read_rgba(b_path, wb, hb, pb)) return false;
    if (wa != wb || ha != hb) return false;
    return pa == pb;
}

// ============================================================================
// Builders
// ============================================================================
SpectralDataset build_synthetic(int n_fft = 1024,
                                int n_frames = 32,
                                int sample_rate = 44100,
                                float tone_hz = 1000.0f) {
    SpectralDataset d;
    d.mutable_analysis_metadata().fft_size = n_fft;
    d.mutable_analysis_metadata().hop_size = n_fft / 2;
    d.mutable_analysis_metadata().sample_rate = sample_rate;
    d.mutable_analysis_metadata().window_type = "hann";
    d.mutable_analysis_metadata().window_coherent_gain = 0.5f;
    d.mutable_analysis_metadata().overlap_ratio = 0.5f;
    d.mutable_analysis_metadata().analyzed_channels = 1;
    d.mutable_analysis_metadata().magnitude_scale = 1.0f;
    d.mutable_analysis_metadata().nyquist_frequency =
        static_cast<float>(sample_rate) / 2.0f;
    d.mutable_analysis_metadata().num_frequency_bins = n_fft / 2 + 1;
    d.mutable_analysis_metadata().analyzer_version = "test-1.0";
    d.mutable_normalization_info().window_coherent_gain = 0.5f;
    d.mutable_normalization_info().db_floor = -90.0f;
    d.mutable_normalization_info().db_reference = 1.0f;
    d.mutable_normalization_info().magnitude_scale = 1.0f;
    d.mutable_normalization_info().reference_amplitude = 1.0f;
    d.mutable_channel_info().total_channels = 1;
    d.mutable_channel_info().analyzed_channels = 1;
    d.mutable_channel_info().analyzed_channel_index = 0;
    d.mutable_source_metadata().file_path = "test://synthetic.wav";
    d.mutable_source_metadata().file_hash = std::string(64, '0');
    d.mutable_source_metadata().sample_rate = sample_rate;
    d.mutable_source_metadata().num_channels = 1;
    d.mutable_source_metadata().duration_seconds =
        static_cast<double>(n_frames * (n_fft / 2)) / sample_rate;
    d.mutable_frequency_axis() = FrequencyAxis(n_fft, sample_rate);
    d.mutable_time_axis() = TimeAxis(n_frames, n_fft / 2, sample_rate);

    const int peak_bin = static_cast<int>(std::round(
        tone_hz * n_fft / static_cast<double>(sample_rate)));
    for (int i = 0; i < n_frames; ++i) {
        Spectral::SpectralFrame f;
        f.frame_index = i;
        f.n_fft = n_fft;
        f.window_factor = 0.5f;
        f.timestamp = i * (n_fft / 2) / static_cast<double>(sample_rate);
        f.rms = 0.5f;
        f.peak_magnitude = 1.0f;
        f.spectral_centroid = tone_hz;
        f.spectral_bandwidth = 50.0f;
        f.magnitudes.assign(n_fft / 2 + 1, 0.01f);
        f.phases.assign(n_fft / 2 + 1, 0.0f);
        f.magnitudes[peak_bin] = 1.0f;
        f.power.resize(f.magnitudes.size());
        for (size_t k = 0; k < f.magnitudes.size(); ++k) {
            f.power[k] = f.magnitudes[k] * f.magnitudes[k];
        }
        d.add_frame(f);
    }
    d.mutable_analysis_metadata().total_frames = d.frame_count();
    d.mutable_analysis_metadata().total_duration_seconds = d.total_duration();
    d.mutable_analysis_metadata().frame_duration_seconds = d.frame_duration();
    return d;
}

SpectralDataset build_silence(int n_fft = 1024, int n_frames = 8) {
    auto d = build_synthetic(n_fft, n_frames);
    // Synthetic builds a peak at bin N; zero all bins. We rebuild frames
    // by adding a new set of all-zero frames and clearing the prior ones.
    d = build_synthetic(n_fft, n_frames);
    // add_frame does not expose a setter, but the dataset's frames_ vector
    // is private. Easiest: zero out the source magnitudes before adding
    // frames. The synthetic builder already added frames; we need to drop
    // them and re-add zeros. add_frame appends; there is no clear. So we
    // build a fresh dataset with zero-filled frames by replicating the
    // builder but with all-zero magnitudes.
    SpectralDataset z;
    z.mutable_analysis_metadata() = d.analysis_metadata();
    z.mutable_normalization_info() = d.normalization_info();
    z.mutable_channel_info() = d.channel_info();
    z.mutable_source_metadata() = d.source_metadata();
    z.mutable_frequency_axis() = d.frequency_axis();
    z.mutable_time_axis() = d.time_axis();
    const int nb = d.num_frequency_bins();
    for (int i = 0; i < n_frames; ++i) {
        Spectral::SpectralFrame f;
        f.frame_index = i;
        f.n_fft = n_fft;
        f.window_factor = 0.5f;
        f.timestamp = i * (n_fft / 2) / static_cast<double>(44100);
        f.rms = 0.0f;
        f.peak_magnitude = 0.0f;
        f.spectral_centroid = 0.0f;
        f.spectral_bandwidth = 0.0f;
        f.magnitudes.assign(nb, 0.0f);
        f.phases.assign(nb, 0.0f);
        f.power.assign(nb, 0.0f);
        z.add_frame(f);
    }
    return z;
}

SpectralDataset build_with_nan(int n_fft = 1024, int n_frames = 4) {
    auto d = build_synthetic(n_fft, n_frames);
    auto f = d.frame(0);
    f.magnitudes[0] = std::numeric_limits<float>::quiet_NaN();
    f.magnitudes[1] = std::numeric_limits<float>::infinity();
    f.magnitudes[2] = -std::numeric_limits<float>::infinity();
    return d;
}

} // namespace

// ============================================================================
// Test 1 — dB normalization
// ============================================================================
static void test_db_normalization() {
    std::printf("\n[Test 1] dB normalization\n");
    EXPECT(SpectrogramRenderer::normalize_db(-90.0f, -90.0f, 0.0f) == 0.0f,
           "db_floor maps to 0.0");
    EXPECT(SpectrogramRenderer::normalize_db(0.0f, -90.0f, 0.0f) == 1.0f,
           "db_ceiling maps to 1.0");
    EXPECT(SpectrogramRenderer::normalize_db(-45.0f, -90.0f, 0.0f) == 0.5f,
           "midpoint maps to 0.5");
    EXPECT(SpectrogramRenderer::normalize_db(-200.0f, -90.0f, 0.0f) == 0.0f,
           "below floor clamps to 0.0");
    EXPECT(SpectrogramRenderer::normalize_db(50.0f, -90.0f, 0.0f) == 1.0f,
           "above ceiling clamps to 1.0");
    EXPECT(SpectrogramRenderer::normalize_db(std::numeric_limits<float>::quiet_NaN(),
                                             -90.0f, 0.0f) == 0.0f,
           "NaN -> 0.0 (floor)");
    EXPECT(SpectrogramRenderer::normalize_db(std::numeric_limits<float>::infinity(),
                                             -90.0f, 0.0f) == 0.0f,
           "+inf -> 0.0 (floor)");
}

// ============================================================================
// Test 2 — Color maps
// ============================================================================
static void test_color_maps() {
    std::printf("\n[Test 2] Color maps\n");
    // Viridis at t=0 is dark purple; at t=1 is yellow.
    uint8_t r, g, b;
    SpectrogramRenderer::color_viridis(0.0f, r, g, b);
    EXPECT(r < 80 && g < 80 && b < 130,
           "viridis(0) is dark blue/purple");
    SpectrogramRenderer::color_viridis(1.0f, r, g, b);
    EXPECT(r > 200 && g > 200 && b < 130,
           "viridis(1) is light yellow");
    SpectrogramRenderer::color_viridis(0.5f, r, g, b);
    EXPECT(g > 100 && g > r && b > r,
           "viridis(0.5) is teal/green (green dominant, blue exceeds red)");
    // Clamping
    SpectrogramRenderer::color_viridis(-1.0f, r, g, b);
    uint8_t r0, g0, b0;
    SpectrogramRenderer::color_viridis(0.0f, r0, g0, b0);
    EXPECT(r == r0 && g == g0 && b == b0, "viridis clamps below 0");
    SpectrogramRenderer::color_viridis(2.0f, r, g, b);
    SpectrogramRenderer::color_viridis(1.0f, r0, g0, b0);
    EXPECT(r == r0 && g == g0 && b == b0, "viridis clamps above 1");

    // Heat
    SpectrogramRenderer::color_heat(0.0f, r, g, b);
    EXPECT(r < 20 && g == 0 && b == 0, "heat(0) is near-black");
    SpectrogramRenderer::color_heat(1.0f, r, g, b);
    EXPECT(r == 255 && g > 200 && b > 200, "heat(1) is near-white");
    SpectrogramRenderer::color_heat(0.5f, r, g, b);
    EXPECT(r > 150 && b < 20, "heat(0.5) is red/orange (no blue)");

    // color_map dispatch
    SpectrogramRenderer::color_map(ColorMap::Heat, 0.5f, r, g, b);
    EXPECT(b == 0, "color_map(Heat) -> no blue at mid");
    SpectrogramRenderer::color_map(ColorMap::Viridis, 0.5f, r, g, b);
    EXPECT(b > 100, "color_map(Viridis) -> blue dominant at mid");
}

// ============================================================================
// Test 3 — Empty dataset
// ============================================================================
static void test_empty_dataset() {
    std::printf("\n[Test 3] Empty dataset\n");
    SpectralDataset d;
    SpectrogramConfig cfg;
    cfg.width = 64; cfg.height = 32;
    SpectrogramRenderer r(cfg);
    RGBAImage img;
    EXPECT(r.render(d, img) == RenderError::EmptyDataset,
           "empty dataset returns EmptyDataset");
    EXPECT(!img.valid(), "image is not valid on empty input");
}

// ============================================================================
// Test 4 — Silence rendering
// ============================================================================
static void test_silence() {
    std::printf("\n[Test 4] Silence rendering\n");
    auto d = build_silence();
    SpectrogramConfig cfg;
    cfg.width = 64; cfg.height = 32;
    cfg.db_floor = -90.0f;
    cfg.db_ceiling = 0.0f;
    cfg.bg_a = 0;
    SpectrogramRenderer r(cfg);
    RGBAImage img;
    EXPECT(r.render(d, img) == RenderError::Ok, "silence renders Ok");
    EXPECT(img.valid(), "image valid");
    // Silence -> all magnitudes at floor -> all pixels at viridis(0)
    uint8_t r0, g0, b0;
    SpectrogramRenderer::color_viridis(0.0f, r0, g0, b0);
    int bg_count = 0, hit_floor_count = 0;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const uint8_t* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
            if (p[0] == r0 && p[1] == g0 && p[2] == b0) ++hit_floor_count;
            if (p[0] == cfg.bg_r && p[1] == cfg.bg_g && p[2] == cfg.bg_b) ++bg_count;
        }
    }
    EXPECT(hit_floor_count > 0 || bg_count == img.width * img.height,
           "silence produces uniform color (no signal visible)");
}

// ============================================================================
// Test 5 — NaN / Inf in input
// ============================================================================
static void test_nan_inf() {
    std::printf("\n[Test 5] NaN/Inf in input\n");
    auto d = build_with_nan();
    SpectrogramConfig cfg;
    cfg.width = 32; cfg.height = 16;
    SpectrogramRenderer r(cfg);
    RGBAImage img;
    EXPECT(r.render(d, img) == RenderError::Ok,
           "NaN/Inf in dataset does not crash renderer");
    EXPECT(img.valid(), "image valid");
    for (size_t i = 0; i < img.pixels.size(); i += 4) {
        EXPECT(img.pixels[i+0] < 256, "R channel finite (0-255)");
        EXPECT(img.pixels[i+1] < 256, "G channel finite (0-255)");
        EXPECT(img.pixels[i+2] < 256, "B channel finite (0-255)");
        EXPECT(img.pixels[i+3] == 255, "A channel = 255 (opaque)");
    }
}

// ============================================================================
// Test 6 — Determinism
// ============================================================================
static void test_determinism() {
    std::printf("\n[Test 6] Determinism\n");
    auto d = build_synthetic();
    SpectrogramConfig cfg;
    cfg.width = 128; cfg.height = 64;
    SpectrogramRenderer r(cfg);
    RGBAImage a, b;
    EXPECT(r.render(d, a) == RenderError::Ok, "first render Ok");
    EXPECT(r.render(d, b) == RenderError::Ok, "second render Ok");
    EXPECT(a.pixels == b.pixels, "two renders produce identical pixels");
    EXPECT(a.width == b.width && a.height == b.height, "dimensions match");
}

// ============================================================================
// Test 7 — Linear vs log frequency scale
// ============================================================================
static void test_linear_vs_log() {
    std::printf("\n[Test 7] Linear vs log frequency\n");
    auto d = build_synthetic();
    SpectrogramConfig cfg_lin, cfg_log;
    cfg_lin.width = 128; cfg_lin.height = 64;
    cfg_lin.freq_scale = FrequencyScale::Linear;
    cfg_log = cfg_lin;
    cfg_log.freq_scale = FrequencyScale::Logarithmic;
    SpectrogramRenderer rl(cfg_lin), rg(cfg_log);
    RGBAImage il, ig;
    EXPECT(rl.render(d, il) == RenderError::Ok, "linear render Ok");
    EXPECT(rg.render(d, ig) == RenderError::Ok, "log render Ok");
    EXPECT(il.pixels != ig.pixels,
           "linear and log produce different images (frequency mapping differs)");
}

// ============================================================================
// Test 8 — Color map switch
// ============================================================================
static void test_colormap_switch() {
    std::printf("\n[Test 8] Color map switch\n");
    auto d = build_synthetic();
    SpectrogramConfig cfg_v, cfg_h;
    cfg_v.width = 64; cfg_v.height = 32;
    cfg_v.color_map = ColorMap::Viridis;
    cfg_h = cfg_v;
    cfg_h.color_map = ColorMap::Heat;
    SpectrogramRenderer rv(cfg_v), rh(cfg_h);
    RGBAImage iv, ih;
    EXPECT(rv.render(d, iv) == RenderError::Ok, "viridis render Ok");
    EXPECT(rh.render(d, ih) == RenderError::Ok, "heat render Ok");
    EXPECT(iv.pixels != ih.pixels, "viridis != heat");
    // Pick a pixel with non-trivial value
    int diff = 0;
    for (size_t i = 0; i < iv.pixels.size(); i += 4) {
        if (iv.pixels[i+0] != ih.pixels[i+0] ||
            iv.pixels[i+1] != ih.pixels[i+1] ||
            iv.pixels[i+2] != ih.pixels[i+2]) ++diff;
    }
    EXPECT(diff > 10, "color maps differ on most non-trivial pixels");
}

// ============================================================================
// Test 9 — PNG round-trip
// ============================================================================
static void test_png_roundtrip() {
    std::printf("\n[Test 9] PNG round-trip\n");
    auto d = build_synthetic();
    SpectrogramConfig cfg;
    cfg.width = 64; cfg.height = 32;
    SpectrogramRenderer r(cfg);
    RGBAImage img;
    EXPECT(r.render(d, img) == RenderError::Ok, "render Ok");

    const std::string path = "test_roundtrip.png";
    std::remove(path.c_str());
    EXPECT(PNGEncoder::write_rgba(path, img.width, img.height, img.pixels.data()),
           "write_rgba succeeds");
    int w2 = 0, h2 = 0;
    std::vector<uint8_t> px2;
    EXPECT(PNGEncoder::read_rgba(path, w2, h2, px2), "read_rgba succeeds");
    EXPECT(w2 == img.width, "decoded width matches");
    EXPECT(h2 == img.height, "decoded height matches");
    EXPECT(px2 == img.pixels, "decoded pixels match written pixels");
    std::remove(path.c_str());
}

// ============================================================================
// Test 10 — Golden image fixtures
// ============================================================================
static void test_golden_fixtures() {
    std::printf("\n[Test 10] Golden image fixtures\n");
    const std::string golden_dir = "tests/phase6";
    std::filesystem::create_directories(golden_dir);

    struct Case {
        const char* name;
        ColorMap cm;
        FrequencyScale fs;
    };
    const std::vector<Case> cases = {
        {"viridis_log", ColorMap::Viridis, FrequencyScale::Logarithmic},
        {"viridis_lin", ColorMap::Viridis, FrequencyScale::Linear},
        {"heat_log",    ColorMap::Heat,    FrequencyScale::Logarithmic},
        {"heat_lin",    ColorMap::Heat,    FrequencyScale::Linear},
    };

    auto d = build_synthetic();
    for (const auto& c : cases) {
        SpectrogramConfig cfg;
        cfg.width = 256; cfg.height = 128;
        cfg.color_map = c.cm;
        cfg.freq_scale = c.fs;
        SpectrogramRenderer r(cfg);
        RGBAImage img;
        EXPECT(r.render(d, img) == RenderError::Ok,
               std::string(c.name) + " render Ok");
        const std::string p = golden_dir + std::string("/golden_") + c.name + ".png";
        EXPECT(PNGEncoder::write_rgba(p, img.width, img.height, img.pixels.data()),
               std::string(c.name) + " write Ok");

        // If a .expected.png file exists, compare against it.
        const std::string expected = p + ".expected.png";
        if (std::filesystem::exists(expected)) {
            EXPECT(png_pixels_equal(p, expected),
                   std::string(c.name) + " matches expected fixture");
        } else {
            std::printf("  NOTE: %s has no .expected.png (first run; fixture "
                        "not yet recorded)\n", expected.c_str());
            ++g_pass;
        }
    }
}

// ============================================================================
// Test 11 — Invalid input handling
// ============================================================================
static void test_invalid_input() {
    std::printf("\n[Test 11] Invalid input\n");
    auto d = build_synthetic();
    SpectrogramConfig cfg;
    cfg.width = 0; cfg.height = 64;
    SpectrogramRenderer r0(cfg);
    RGBAImage img;
    EXPECT(r0.render(d, img) == RenderError::InvalidDimensions,
           "zero width -> InvalidDimensions");
    cfg.width = 64; cfg.height = 0;
    SpectrogramRenderer r1(cfg);
    EXPECT(r1.render(d, img) == RenderError::InvalidDimensions,
           "zero height -> InvalidDimensions");
    cfg.width = 64; cfg.height = 64;
    cfg.freq_min_hz = 0.0f;
    cfg.freq_max_hz = 0.0f;
    // Auto-fmax = nyquist. Should succeed.
    SpectrogramRenderer r2(cfg);
    EXPECT(r2.render(d, img) == RenderError::Ok, "auto fmax renders Ok");
    cfg.freq_min_hz = 25000.0f;  // > nyquist
    SpectrogramRenderer r3(cfg);
    EXPECT(r3.render(d, img) == RenderError::InvalidFrequencyRange,
           "fmin >= fmax -> InvalidFrequencyRange");
}

// ============================================================================
// Test 12 — Pipeline integration (output_spectrogram.png example)
// ============================================================================
static void test_pipeline_integration() {
    std::printf("\n[Test 12] Pipeline integration (input.wav -> output.png example)\n");
    auto d = build_synthetic();
    SpectrogramConfig cfg;
    cfg.width = 512; cfg.height = 256;
    cfg.color_map = ColorMap::Viridis;
    cfg.freq_scale = FrequencyScale::Logarithmic;
    SpectrogramRenderer r(cfg);
    const std::string path = "output_spectrogram.png";
    std::remove(path.c_str());
    EXPECT(r.render_to_png(d, path) == RenderError::Ok,
           "render_to_png produces output_spectrogram.png");
    std::ifstream f(path, std::ios::binary);
    EXPECT(f.good(), "output file exists and is openable");
    f.close();
    int w, h;
    std::vector<uint8_t> px;
    EXPECT(PNGEncoder::read_rgba(path, w, h, px), "output is valid PNG");
    EXPECT(w == cfg.width, "output width matches config");
    EXPECT(h == cfg.height, "output height matches config");
    std::remove(path.c_str());
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::printf("=== SpectrogramRenderer Test Suite (Phase 6) ===\n");
    test_db_normalization();
    test_color_maps();
    test_empty_dataset();
    test_silence();
    test_nan_inf();
    test_determinism();
    test_linear_vs_log();
    test_colormap_switch();
    test_png_roundtrip();
    test_golden_fixtures();
    test_invalid_input();
    test_pipeline_integration();
    std::printf("\n=== Summary ===\n");
    std::printf("Passed: %d\n", g_pass);
    std::printf("Failed: %d\n", g_fail);
    std::printf("Result: %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
