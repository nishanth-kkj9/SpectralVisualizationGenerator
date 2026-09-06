#include "video_renderer.h"
#include "spectral_dataset.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

// Build a synthetic dataset: 440 Hz sine, 2 seconds, 1024 FFT, 44100 Hz
static Spectral::SpectralDataset make_test_dataset() {
    using namespace Spectral;
    SpectralDataset ds;
    const int sr = 44100;
    const int fft_size = 1024;
    const int hop = 512;
    const int num_frames = 80; // ~0.93 seconds at hop=512, sr=44100

    ds.mutable_frequency_axis() = FrequencyAxis(fft_size, sr);

    for (int i = 0; i < num_frames; ++i) {
        Spectral::SpectralFrame sf;
        sf.frame_index = i;
        sf.n_fft = fft_size;
        sf.window_factor = 0.5f;
        sf.timestamp = static_cast<double>(i * hop) / sr;
        sf.magnitudes.resize(fft_size / 2 + 1);
        sf.phases.resize(fft_size / 2 + 1);
        sf.power.resize(fft_size / 2 + 1);

        // Put energy at bin ~18 (approx 778 Hz for 1024 FFT at 44100)
        for (int k = 0; k < fft_size / 2 + 1; ++k) {
            float dist = std::abs(k - 18.0f);
            float mag = std::exp(-dist * dist / 8.0f) * 0.5f;
            sf.magnitudes[k] = mag;
            sf.power[k] = mag * mag;
        }
        sf.rms = 0.1f;
        sf.peak_magnitude = 0.5f;
        sf.spectral_centroid = 778.0f;
        ds.add_frame(sf);
    }

    ds.mutable_time_axis() = TimeAxis(num_frames, hop, sr);
    auto& am = ds.mutable_analysis_metadata();
    am.fft_size = fft_size;
    am.hop_size = hop;
    am.sample_rate = sr;

    return ds;
}

static void test_render_frame_dimensions() {
    std::fprintf(stderr, "[test_render_frame_dimensions]\n");
    auto ds = make_test_dataset();
    Spectral::VideoRendererConfig cfg;
    cfg.width = 128;
    cfg.height = 64;
    cfg.window_seconds = 1.0f;
    Spectral::VideoRenderer vr(cfg);

    Spectral::RGBAImage img;
    auto err = vr.render_frame(ds, 0.5, img);
    CHECK(err == Spectral::VideoRenderError::Ok, "render_frame succeeds");
    CHECK(img.width == 128, "width matches");
    CHECK(img.height == 64, "height matches");
    CHECK(img.pixels.size() == 128 * 64 * 4, "pixel buffer size");
}

static void test_render_frame_different_times() {
    std::fprintf(stderr, "[test_render_frame_different_times]\n");
    auto ds = make_test_dataset();
    Spectral::VideoRendererConfig cfg;
    cfg.window_seconds = 0.5f;
    Spectral::VideoRenderer vr(cfg);

    Spectral::RGBAImage img1, img2;
    vr.render_frame(ds, 0.1, img1);
    vr.render_frame(ds, 0.5, img2);

    // Different times should produce different images
    CHECK(img1.pixels != img2.pixels, "different times produce different frames");
}

static void test_render_frame_empty() {
    std::fprintf(stderr, "[test_render_frame_empty]\n");
    Spectral::SpectralDataset empty;
    Spectral::VideoRenderer vr;
    Spectral::RGBAImage img;
    auto err = vr.render_frame(empty, 0.0, img);
    CHECK(err == Spectral::VideoRenderError::EmptyDataset, "empty dataset returns error");
}

static void test_render_video() {
    std::fprintf(stderr, "[test_render_video]\n");
    auto ds = make_test_dataset();
    Spectral::VideoRendererConfig cfg;
    cfg.width = 64;
    cfg.height = 32;
    cfg.fps = 5;
    cfg.codec = "libx264";
    cfg.crf = 28;
    cfg.window_seconds = 0.5f;
    Spectral::VideoRenderer vr(cfg);

    auto err = vr.render(ds, "test_video_out.mp4");
    CHECK(err == Spectral::VideoRenderError::Ok, "render to video succeeds");
    CHECK(fs::exists("test_video_out.mp4"), "output file exists");
    CHECK(fs::file_size("test_video_out.mp4") > 100, "output has content");
    fs::remove("test_video_out.mp4");
}

static void test_render_different_fps() {
    std::fprintf(stderr, "[test_render_different_fps]\n");
    auto ds = make_test_dataset();

    for (int fps : {5, 10, 24}) {
        Spectral::VideoRendererConfig cfg;
        cfg.width = 32;
        cfg.height = 16;
        cfg.fps = fps;
        cfg.codec = "libx264";
        cfg.crf = 28;
        cfg.window_seconds = 0.3f;
        Spectral::VideoRenderer vr(cfg);

        std::string path = "test_fps_" + std::to_string(fps) + ".mp4";
        auto err = vr.render(ds, path);
        CHECK(err == Spectral::VideoRenderError::Ok, "render fps ok");
        CHECK(fs::exists(path), "file exists for fps");
        fs::remove(path);
    }
}

int main() {
    test_render_frame_dimensions();
    test_render_frame_different_times();
    test_render_frame_empty();
    test_render_video();
    test_render_different_fps();

    std::fprintf(stderr, "\n=== video_renderer: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
