// Phase 8 — ProjectConfig reproducibility tests
// - Round-trip: same ProjectConfig produces same analytical configuration.
// - Determinism: same input + same config -> same bytes (spectral, image).
// - Golden: detect drift, do not auto-overwrite.
// - Video tier is explicitly NOT claimed byte-identical.

#include "project_config.h"
#include "spectral_dataset.h"
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

// SHA256 hex of a file's bytes.
static std::string file_sha256(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    // Use the same SHA256 impl via ProjectConfig (it hashes the input)
    // Instead inline a tiny SHA256 here:
    // Fall back to a simple non-crypto checksum for the test;
    // reproducibility is asserted via ProjectConfig::fingerprint() later.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return std::string(buf);
}

// Checksum over rendered image (FNV-1a 64).
static uint64_t checksum(const RGBAImage& img) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < img.pixels.size(); ++i) {
        h ^= img.pixels[i];
        h *= 1099511628211ull;
    }
    return h;
}

static ProjectConfig make_default_config() {
    ProjectConfig c;
    c.input.file_path = "test.wav";
    c.input.file_hash = "deadbeef";
    c.input.file_size_bytes = 1024;
    c.input.sample_rate = 44100;
    c.input.num_channels = 1;
    c.input.duration_seconds = 1.0;
    c.input.codec_name = "pcm_s16le";
    c.input.codec_long_name = "PCM signed 16-bit little-endian";
    c.analysis.fft_size = 1024;
    c.analysis.hop_size = 256;
    c.analysis.overlap_ratio = 0.75f;
    c.analysis.window_type = ProjectWindowType::Hann;
    c.analysis.window_coherent_gain = 0.5f;
    c.analysis.sample_rate = 44100;
    c.analysis.analyzed_channels = 1;
    c.analysis.channel_mapping = 0;
    c.analysis.magnitude_scale = 1.0f;
    c.analysis.phase_unwrap = 0.0f;
    c.dynamic_range.db_floor = -90.0f;
    c.dynamic_range.db_ceiling = 0.0f;
    c.dynamic_range.reference_amplitude = 1.0f;
    c.dynamic_range.window_energy_gain = 0.375f;
    c.frequency_range.min_hz = 20.0f;
    c.frequency_range.max_hz = 20000.0f;
    c.frequency_range.scale = ProjectFreqScale::Logarithmic;
    c.renderer.kind = RendererKind::Spectrum;
    c.renderer.color_map = ProjectColorMap::Viridis;
    c.renderer.width = 800;
    c.renderer.height = 400;
    c.renderer.dpi = 96;
    c.renderer.draw_grid = true;
    c.renderer.draw_labels = true;
    c.renderer.line_thickness = 2;
    c.renderer.grid_divisions_x = 8;
    c.renderer.grid_divisions_y = 6;
    c.project_id = "test-project-001";
    c.created_utc = "2026-09-02T00:00:00Z";
    c.notes = "Phase 8 reproducible test";
    return c;
}

// Build a synthetic SpectralDataset with a single tone at given bin.
static Spectral::SpectralDataset build_synthetic_dataset(
    int n_fft, int sr, int peak_bin, float amplitude, int n_frames = 1)
{
    Spectral::SpectralDataset d;
    d.mutable_analysis_metadata().fft_size = n_fft;
    d.mutable_analysis_metadata().hop_size = n_fft / 2;
    d.mutable_analysis_metadata().sample_rate = sr;
    d.mutable_analysis_metadata().nyquist_frequency = (float)(sr / 2.0);
    d.mutable_analysis_metadata().num_frequency_bins = n_fft / 2 + 1;
    d.mutable_analysis_metadata().window_type = "hann";
    d.mutable_analysis_metadata().window_coherent_gain = 0.5f;
    d.mutable_frequency_axis() = Spectral::FrequencyAxis(n_fft, sr);
    d.mutable_time_axis() = Spectral::TimeAxis(n_frames, n_fft / 2, sr);
    const int nb = n_fft / 2 + 1;
    for (int i = 0; i < n_frames; ++i) {
        Spectral::SpectralFrame f;
        f.n_fft = n_fft;
        f.frame_index = i;
        f.timestamp = (double)i * (double)(n_fft / 2) / (double)sr;
        f.magnitudes.assign(nb, 0.0f);
        if (peak_bin >= 0 && peak_bin < nb) f.magnitudes[peak_bin] = amplitude;
        d.add_frame(f);
    }
    return d;
}

// ============================================================================

static void test_default_validate() {
    std::fprintf(stderr, "[default_validate]\n");
    auto c = make_default_config();
    std::vector<std::string> errs;
    CHECK(c.validate(errs));
    CHECK(errs.empty());
}

static void test_rejects_invalid_fft_size() {
    std::fprintf(stderr, "[rejects_invalid_fft_size]\n");
    auto c = make_default_config();
    c.analysis.fft_size = 1000;  // not power of two
    std::vector<std::string> errs;
    CHECK(!c.validate(errs));
    CHECK(!errs.empty());
}

static void test_rejects_bad_hop() {
    std::fprintf(stderr, "[rejects_bad_hop]\n");
    auto c = make_default_config();
    c.analysis.hop_size = 0;
    std::vector<std::string> errs;
    CHECK(!c.validate(errs));
    c.analysis.hop_size = 2048;  // > fft_size
    errs.clear();
    CHECK(!c.validate(errs));
}

static void test_rejects_inverted_db() {
    std::fprintf(stderr, "[rejects_inverted_db]\n");
    auto c = make_default_config();
    c.dynamic_range.db_floor = 0.0f;
    c.dynamic_range.db_ceiling = -90.0f;
    std::vector<std::string> errs;
    CHECK(!c.validate(errs));
}

static void test_json_round_trip_equality() {
    std::fprintf(stderr, "[json_round_trip_equality]\n");
    auto c = make_default_config();
    std::string j1 = ProjectConfigSerializer::to_json(c);
    ProjectConfig c2;
    std::string err;
    bool ok = ProjectConfigSerializer::from_json(j1, c2, err);
    if (!ok) {
        std::fprintf(stderr, "  from_json FAILED: %s\n", err.c_str());
        std::fprintf(stderr, "  JSON (first 200): ");
        for (size_t i = 0; i < std::min(j1.size(), (size_t)200); ++i) {
            char ch = j1[i];
            if (ch == '\n') std::fprintf(stderr, "\\n");
            else std::fprintf(stderr, "%c", ch);
        }
        std::fprintf(stderr, "\n");
    }
    CHECK(ok);
    CHECK(err.empty());
    CHECK(c == c2);
    // Round-trip again: bytes should be identical (canonical form).
    std::string j2 = ProjectConfigSerializer::to_json(c2);
    CHECK(j1 == j2);
}

static void test_fingerprint_stable() {
    std::fprintf(stderr, "[fingerprint_stable]\n");
    auto c = make_default_config();
    auto f1 = c.fingerprint();
    auto f2 = c.fingerprint();
    CHECK(f1 == f2);
    CHECK_EQ(f1.size(), (size_t)64);  // SHA256 hex
}

static void test_fingerprint_changes_with_fft() {
    std::fprintf(stderr, "[fingerprint_changes_with_fft]\n");
    auto c1 = make_default_config();
    auto c2 = c1; c2.analysis.fft_size = 2048;
    CHECK(c1.fingerprint() != c2.fingerprint());
}

static void test_fingerprint_changes_with_renderer() {
    std::fprintf(stderr, "[fingerprint_changes_with_renderer]\n");
    auto c1 = make_default_config();
    auto c2 = c1; c2.renderer.width = 1024;
    CHECK(c1.fingerprint() != c2.fingerprint());
}

static void test_canonical_key_order() {
    std::fprintf(stderr, "[canonical_key_order]\n");
    auto c = make_default_config();
    std::string j = ProjectConfigSerializer::to_json(c);
    // Check first few top-level keys are sorted
    auto pos_a = j.find("\"analysis\":");
    auto pos_d = j.find("\"dynamic_range\":");
    auto pos_f = j.find("\"frequency_range\":");
    auto pos_i = j.find("\"input\":");
    auto pos_r = j.find("\"renderer\":");
    CHECK(pos_a != std::string::npos);
    CHECK(pos_d != std::string::npos);
    CHECK(pos_f != std::string::npos);
    CHECK(pos_i != std::string::npos);
    CHECK(pos_r != std::string::npos);
    CHECK(pos_a < pos_d);  // analysis < dynamic_range
    CHECK(pos_d < pos_i);  // dynamic_range < input
    CHECK(pos_f < pos_i);  // frequency_range < input
    CHECK(pos_f < pos_r);  // frequency_range < renderer
}

static void test_save_load_file() {
    std::fprintf(stderr, "[save_load_file]\n");
    fs::create_directories("tests/golden");
    auto c = make_default_config();
    const std::string path = "tests/golden/_round_trip.json";
    std::string err;
    CHECK(ProjectConfigSerializer::save(path, c, err, /*pretty*/true));
    CHECK(err.empty());
    CHECK(fs::exists(path));
    ProjectConfig c2;
    CHECK(ProjectConfigSerializer::load(path, c2, err));
    CHECK(err.empty());
    CHECK(c == c2);
}

static void test_strict_parser_rejects_unknown() {
    std::fprintf(stderr, "[strict_parser_rejects_unknown]\n");
    auto c = make_default_config();
    std::string j = ProjectConfigSerializer::to_json(c);
    // Inject an unknown top-level field
    std::string bad = "{\"extra_field\":42," + j.substr(1);
    ProjectConfig c2;
    std::string err;
    bool ok = ProjectConfigSerializer::from_json(bad, c2, err);
    if (ok) {
        std::fprintf(stderr, "  BUG: strict mode accepted unknown field (should reject)\n");
    }
    CHECK(!ok);
    CHECK(!err.empty());
    // But allow_unknown=true should accept it
    err.clear();
    ProjectConfig c3;
    bool ok2 = ProjectConfigSerializer::from_json(bad, c3, err, /*allow_unknown*/true);
    if (!ok2) {
        std::fprintf(stderr, "  allow_unknown=true failed: '%s'\n", err.c_str());
        std::fprintf(stderr, "  bad JSON: %s\n", bad.substr(0, 120).c_str());
    }
    CHECK(ok2);
    CHECK(err.empty());
}

static void test_pretty_json_validates() {
    std::fprintf(stderr, "[pretty_json_validates]\n");
    auto c = make_default_config();
    std::string pretty = ProjectConfigSerializer::to_json_pretty(c);
    CHECK(pretty.find('\n') != std::string::npos);
    ProjectConfig c2;
    std::string err;
    CHECK(ProjectConfigSerializer::from_json(pretty, c2, err));
    CHECK(c == c2);
}

static void test_adapter_to_analysis_metadata() {
    std::fprintf(stderr, "[adapter_to_analysis_metadata]\n");
    auto c = make_default_config();
    AnalysisMetadata m;
    ProjectConfigAdapter::to_analysis_metadata(c, m);
    CHECK_EQ(m.fft_size, c.analysis.fft_size);
    CHECK_EQ(m.hop_size, c.analysis.hop_size);
    CHECK_NEAR(m.overlap_ratio, c.analysis.overlap_ratio, 1e-9);
    CHECK_EQ(m.sample_rate, c.analysis.sample_rate);
    CHECK(m.window_type == std::string("hann"));
    CHECK_NEAR(m.window_coherent_gain, c.analysis.window_coherent_gain, 1e-9);
    CHECK_NEAR(m.magnitude_scale, c.analysis.magnitude_scale, 1e-9);
    CHECK_EQ(m.analyzed_channels, c.analysis.analyzed_channels);
    CHECK_EQ(m.channel_mapping, c.analysis.channel_mapping);
    CHECK_NEAR(m.nyquist_frequency, c.analysis.sample_rate / 2.0f, 1e-6);
    CHECK_EQ(m.num_frequency_bins, c.analysis.fft_size / 2 + 1);
}

static void test_adapter_to_spectrum_config() {
    std::fprintf(stderr, "[adapter_to_spectrum_config]\n");
    auto c = make_default_config();
    c.renderer.kind = RendererKind::Spectrum;
    SpectrumConfig sc;
    ProjectConfigAdapter::to_spectrum_config(c, sc);
    CHECK_EQ(sc.width, c.renderer.width);
    CHECK_EQ(sc.height, c.renderer.height);
    CHECK_NEAR(sc.db_floor, c.dynamic_range.db_floor, 1e-9);
    CHECK_NEAR(sc.db_ceiling, c.dynamic_range.db_ceiling, 1e-9);
    CHECK_NEAR(sc.freq_min_hz, c.frequency_range.min_hz, 1e-9);
    CHECK_NEAR(sc.freq_max_hz, c.frequency_range.max_hz, 1e-9);
    // freq_scale is mapped
    CHECK((int)sc.freq_scale == (int)c.frequency_range.scale);
    // color_map is mapped
    CHECK((int)sc.color_map == (int)c.renderer.color_map);
    CHECK_EQ((int)sc.draw_grid, (int)c.renderer.draw_grid);
    CHECK_EQ((int)sc.draw_labels, (int)c.renderer.draw_labels);
    CHECK_EQ(sc.line_thickness, c.renderer.line_thickness);
}

static void test_adapter_to_spectrogram_config() {
    std::fprintf(stderr, "[adapter_to_spectrogram_config]\n");
    auto c = make_default_config();
    c.renderer.kind = RendererKind::Spectrogram;
    SpectrogramConfig sc;
    ProjectConfigAdapter::to_spectrogram_config(c, sc);
    CHECK_EQ(sc.width, c.renderer.width);
    CHECK_EQ(sc.height, c.renderer.height);
    CHECK_NEAR(sc.db_floor, c.dynamic_range.db_floor, 1e-9);
    CHECK_NEAR(sc.db_ceiling, c.dynamic_range.db_ceiling, 1e-9);
    CHECK_NEAR(sc.freq_min_hz, c.frequency_range.min_hz, 1e-9);
    CHECK_NEAR(sc.freq_max_hz, c.frequency_range.max_hz, 1e-9);
    CHECK((int)sc.freq_scale == (int)c.frequency_range.scale);
    CHECK((int)sc.color_map == (int)c.renderer.color_map);
}

static void test_same_config_same_render_bytes() {
    std::fprintf(stderr, "[same_config_same_render_bytes]\n");
    auto c = make_default_config();
    auto d = build_synthetic_dataset(c.analysis.fft_size, c.analysis.sample_rate,
                                      100, 0.5f, 4);
    SpectrumConfig sc;
    ProjectConfigAdapter::to_spectrum_config(c, sc);
    SpectrumRenderer r1(sc), r2(sc);
    RGBAImage a, b;
    CHECK_EQ((int)r1.render(d, a), (int)SpectrumError::Ok);
    CHECK_EQ((int)r2.render(d, b), (int)SpectrumError::Ok);
    CHECK_EQ(checksum(a), checksum(b));
    CHECK(a.pixels == b.pixels);
}

static void test_same_config_same_aggregate() {
    std::fprintf(stderr, "[same_config_same_aggregate]\n");
    auto c = make_default_config();
    auto d = build_synthetic_dataset(c.analysis.fft_size, c.analysis.sample_rate,
                                      100, 0.5f, 4);
    auto s1 = SpectrumRenderer::aggregate(d, SpectrumAggregation::Mean);
    auto s2 = SpectrumRenderer::aggregate(d, SpectrumAggregation::Mean);
    CHECK(s1 == s2);
    CHECK_NEAR(s1[100], 0.5f, 1e-6);
}

static void test_different_configs_different_fingerprints() {
    std::fprintf(stderr, "[different_configs_different_fingerprints]\n");
    auto base = make_default_config();
    // Vary each meaningful field and ensure fingerprint changes
    auto vary = base; vary.analysis.fft_size = 2048;
    CHECK(base.fingerprint() != vary.fingerprint());
    vary = base; vary.analysis.hop_size = 512;
    CHECK(base.fingerprint() != vary.fingerprint());
    vary = base; vary.analysis.window_type = ProjectWindowType::Hamming;
    CHECK(base.fingerprint() != vary.fingerprint());
    vary = base; vary.dynamic_range.db_floor = -60.0f;
    CHECK(base.fingerprint() != vary.fingerprint());
    vary = base; vary.frequency_range.min_hz = 10.0f;
    CHECK(base.fingerprint() != vary.fingerprint());
    vary = base; vary.frequency_range.scale = ProjectFreqScale::Linear;
    CHECK(base.fingerprint() != vary.fingerprint());
    vary = base; vary.renderer.color_map = ProjectColorMap::Heat;
    CHECK(base.fingerprint() != vary.fingerprint());
    vary = base; vary.renderer.width = 1024;
    CHECK(base.fingerprint() != vary.fingerprint());
    vary = base; vary.renderer.kind = RendererKind::Spectrogram;
    CHECK(base.fingerprint() != vary.fingerprint());
    vary = base; vary.input.file_hash = "different";
    CHECK(base.fingerprint() != vary.fingerprint());
    vary = base; vary.notes = "different";
    CHECK(base.fingerprint() != vary.fingerprint());
    vary = base; vary.project_id = "different";
    CHECK(base.fingerprint() != vary.fingerprint());
}

static void test_reproducibility_tier_field() {
    std::fprintf(stderr, "[reproducibility_tier_field]\n");
    auto c = make_default_config();
    std::string j = ProjectConfigSerializer::to_json(c);
    CHECK(j.find("\"reproducibility_tier\":\"spectral_image_not_video\"") != std::string::npos);
    c.reproducibility_tier = ReproducibilityTier::None;
    j = ProjectConfigSerializer::to_json(c);
    CHECK(j.find("\"reproducibility_tier\":\"none\"") != std::string::npos);
}

static void test_video_tier_documented() {
    std::fprintf(stderr, "[video_tier_documented]\n");
    // The schema_version and software_version are recorded but this test
    // asserts that the schema DOES NOT claim byte-identical video.
    // We do this by ensuring the reproducibility_tier string is the
    // documented one that does not include "video_byte_identical".
    auto c = make_default_config();
    const std::string j = ProjectConfigSerializer::to_json(c);
    CHECK(j.find("video_byte_identical") == std::string::npos);
    CHECK(j.find("byte_identical_video") == std::string::npos);
}

static void test_schema_version_constant() {
    std::fprintf(stderr, "[schema_version_constant]\n");
    auto c = make_default_config();
    std::string j = ProjectConfigSerializer::to_json(c);
    // Must contain schema_version
    CHECK(j.find("\"schema_version\":1") != std::string::npos);
    CHECK_EQ(c.schema_version, (uint32_t)PROJECT_CONFIG_SCHEMA_VERSION);
}

static void test_old_schema_rejected() {
    std::fprintf(stderr, "[old_schema_rejected]\n");
    auto c = make_default_config();
    c.schema_version = PROJECT_CONFIG_MIN_COMPATIBLE_VERSION - 1;
    std::string j = ProjectConfigSerializer::to_json(c);
    ProjectConfig c2;
    std::string err;
    bool ok = ProjectConfigSerializer::from_json(j, c2, err);
    CHECK(!ok);
    CHECK(!err.empty());
}

static void test_golden_reproducible_render() {
    std::fprintf(stderr, "[golden_reproducible_render]\n");
    fs::create_directories("tests/golden");
    auto c = make_default_config();
    auto d = build_synthetic_dataset(c.analysis.fft_size, c.analysis.sample_rate,
                                      100, 0.5f, 4);
    SpectrumConfig sc;
    ProjectConfigAdapter::to_spectrum_config(c, sc);
    SpectrumRenderer r(sc);
    RGBAImage img;
    CHECK_EQ((int)r.render(d, img), (int)SpectrumError::Ok);

    const std::string png_path = "tests/golden/golden_spectrum_v1.png";
    CHECK(PNGEncoder::write_rgba(png_path, img.width, img.height, img.pixels.data()));

    const std::string expected = png_path + ".expected.png";
    if (fs::exists(expected)) {
        int w, h;
        std::vector<uint8_t> px;
        if (PNGEncoder::read_rgba(expected, w, h, px)) {
            CHECK_EQ(w, img.width);
            CHECK_EQ(h, img.height);
            uint64_t ex_h = 1469598103934665603ull;
            for (auto v : px) { ex_h ^= v; ex_h *= 1099511628211ull; }
            if (ex_h != checksum(img)) {
                std::fprintf(stderr,
                    "  REPORT: golden fixture drift detected.\n"
                    "  current:  %s\n"
                    "  expected: %s\n"
                    "  Investigate and update the reference manually; do not\n"
                    "  overwrite %s automatically.\n",
                    png_path.c_str(), expected.c_str(), expected.c_str());
            }
            CHECK_EQ(ex_h, checksum(img));
        }
    }
}

static void test_png_serialization_deterministic() {
    std::fprintf(stderr, "[png_serialization_deterministic]\n");
    auto c = make_default_config();
    auto d = build_synthetic_dataset(c.analysis.fft_size, c.analysis.sample_rate,
                                      100, 0.5f, 4);
    SpectrumConfig sc;
    ProjectConfigAdapter::to_spectrum_config(c, sc);
    SpectrumRenderer r(sc);
    RGBAImage img;
    CHECK_EQ((int)r.render(d, img), (int)SpectrumError::Ok);

    fs::create_directories("tests/golden");
    const std::string a_path = "tests/golden/_det_a.png";
    const std::string b_path = "tests/golden/_det_b.png";
    CHECK(PNGEncoder::write_rgba(a_path, img.width, img.height, img.pixels.data()));
    CHECK(PNGEncoder::write_rgba(b_path, img.width, img.height, img.pixels.data()));
    // Two serializations of the same RGBA buffer must produce identical files.
    CHECK(file_sha256(a_path) == file_sha256(b_path));
}

static void test_load_corrupt_json_fails() {
    std::fprintf(stderr, "[load_corrupt_json_fails]\n");
    ProjectConfig c;
    std::string err;
    CHECK(!ProjectConfigSerializer::from_json("{not valid json", c, err));
    CHECK(!err.empty());
}

static void test_load_truncated_fails() {
    std::fprintf(stderr, "[load_truncated_fails]\n");
    auto c = make_default_config();
    std::string j = ProjectConfigSerializer::to_json(c);
    // Truncate
    ProjectConfig c2;
    std::string err;
    CHECK(!ProjectConfigSerializer::from_json(j.substr(0, j.size() / 2), c2, err));
    CHECK(!err.empty());
}

static void test_window_type_round_trip() {
    std::fprintf(stderr, "[window_type_round_trip]\n");
    for (auto w : {ProjectWindowType::Rectangular, ProjectWindowType::Hann,
                   ProjectWindowType::Hamming, ProjectWindowType::Blackman}) {
        auto c = make_default_config();
        c.analysis.window_type = w;
        std::string j = ProjectConfigSerializer::to_json(c);
        ProjectConfig c2;
        std::string err;
        CHECK(ProjectConfigSerializer::from_json(j, c2, err));
        CHECK_EQ((int)c2.analysis.window_type, (int)w);
    }
}

static void test_color_map_round_trip() {
    std::fprintf(stderr, "[color_map_round_trip]\n");
    for (auto m : {ProjectColorMap::Viridis, ProjectColorMap::Heat}) {
        auto c = make_default_config();
        c.renderer.color_map = m;
        std::string j = ProjectConfigSerializer::to_json(c);
        ProjectConfig c2;
        std::string err;
        CHECK(ProjectConfigSerializer::from_json(j, c2, err));
        CHECK_EQ((int)c2.renderer.color_map, (int)m);
    }
}

static void test_full_pipeline_reproducible() {
    std::fprintf(stderr, "[full_pipeline_reproducible]\n");
    // Construct config, build a dataset from a deterministic buffer
    // (caller-side analysis is out of scope for this test; we use a fixed
    // synthetic dataset as the analysis "output"), then render via the
    // adapter, and confirm bytes are stable across two runs.
    auto c = make_default_config();
    auto d = build_synthetic_dataset(c.analysis.fft_size, c.analysis.sample_rate,
                                      100, 0.5f, 4);

    auto run_pipeline = [&]() -> uint64_t {
        SpectrumConfig sc;
        ProjectConfigAdapter::to_spectrum_config(c, sc);
        SpectrumRenderer r(sc);
        RGBAImage img;
        if (r.render(d, img) != SpectrumError::Ok) return 0;
        return checksum(img);
    };
    uint64_t h1 = run_pipeline();
    uint64_t h2 = run_pipeline();
    CHECK(h1 != 0);
    CHECK_EQ(h1, h2);
}

int main() {
    test_default_validate();
    test_rejects_invalid_fft_size();
    test_rejects_bad_hop();
    test_rejects_inverted_db();
    test_json_round_trip_equality();
    test_fingerprint_stable();
    test_fingerprint_changes_with_fft();
    test_fingerprint_changes_with_renderer();
    test_canonical_key_order();
    test_save_load_file();
    test_strict_parser_rejects_unknown();
    test_pretty_json_validates();
    test_adapter_to_analysis_metadata();
    test_adapter_to_spectrum_config();
    test_adapter_to_spectrogram_config();
    test_same_config_same_render_bytes();
    test_same_config_same_aggregate();
    test_different_configs_different_fingerprints();
    test_reproducibility_tier_field();
    test_video_tier_documented();
    test_schema_version_constant();
    test_old_schema_rejected();
    test_golden_reproducible_render();
    test_png_serialization_deterministic();
    test_load_corrupt_json_fails();
    test_load_truncated_fails();
    test_window_type_round_trip();
    test_color_map_round_trip();
    test_full_pipeline_reproducible();

    std::fprintf(stderr, "\n=== project_config: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
