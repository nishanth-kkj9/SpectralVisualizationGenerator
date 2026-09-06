#include "video_encoder.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

static void test_not_open() {
    std::fprintf(stderr, "[test_not_open]\n");
    Spectral::VideoEncoder enc;
    CHECK(!enc.is_open(), "starts not open");
    std::vector<uint8_t> px(64 * 64 * 4, 128);
    auto err = enc.write_frame(px.data(), 0);
    CHECK(err == Spectral::VideoEncoderError::NotOpen, "write_frame returns NotOpen");
}

static void test_open_close() {
    std::fprintf(stderr, "[test_open_close]\n");
    Spectral::VideoEncoder enc;
    Spectral::VideoEncoderConfig cfg;
    cfg.width = 64;
    cfg.height = 64;
    cfg.fps = 10;
    cfg.codec = "libx264";
    cfg.crf = 28;
    auto err = enc.open("test_enc_output.mp4", cfg);
    CHECK(err == Spectral::VideoEncoderError::Ok, "open succeeds");
    CHECK(enc.is_open(), "is_open after open");
    err = enc.close();
    CHECK(err == Spectral::VideoEncoderError::Ok, "close succeeds");
    CHECK(!enc.is_open(), "not open after close");
    fs::remove("test_enc_output.mp4");
}

static void test_write_frames() {
    std::fprintf(stderr, "[test_write_frames]\n");
    Spectral::VideoEncoder enc;
    Spectral::VideoEncoderConfig cfg;
    cfg.width = 32;
    cfg.height = 32;
    cfg.fps = 5;
    cfg.codec = "libx264";
    cfg.crf = 28;
    auto err = enc.open("test_enc_frames.mp4", cfg);
    CHECK(err == Spectral::VideoEncoderError::Ok, "open");

    std::vector<uint8_t> px(32 * 32 * 4);
    for (int i = 0; i < 10; ++i) {
        // Fill with gradient based on frame index
        for (size_t j = 0; j < px.size(); j += 4) {
            px[j] = static_cast<uint8_t>(i * 25);     // R
            px[j+1] = static_cast<uint8_t>(j % 256);  // G
            px[j+2] = 100;                              // B
            px[j+3] = 255;                              // A
        }
        err = enc.write_frame(px.data(), i);
        CHECK(err == Spectral::VideoEncoderError::Ok, "write_frame");
    }

    err = enc.close();
    CHECK(err == Spectral::VideoEncoderError::Ok, "close after writes");
    CHECK(fs::exists("test_enc_frames.mp4"), "output file exists");
    CHECK(fs::file_size("test_enc_frames.mp4") > 100, "output has content");
    fs::remove("test_enc_frames.mp4");
}

int main() {
    test_not_open();
    test_open_close();
    test_write_frames();

    std::fprintf(stderr, "\n=== video_encoder: %d/%d passed ===\n",
                 tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
