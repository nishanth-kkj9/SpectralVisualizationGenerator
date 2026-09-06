// tests/audio/test_audio_buffer.cpp
// Unit tests for AudioBuffer::mix_down (per-frame mean) and resample
// (linear interpolation). Expectations are hand-computed, never produced
// by calling the implementation under test.
#include "audio_buffer.h"

#include <cmath>
#include <cstdio>
#include <vector>

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

static bool near(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

// The S3 regression case: left=[1,2,3,4], right=[5,6,7,8] must give
// [3,4,5,6]. The old code produced a DC constant instead.
static void test_mix_down_exact() {
    std::printf("[mix_down_exact]\n");
    AudioBuffer buf(44100, 2);
    buf.data = {1, 5, 2, 6, 3, 7, 4, 8};  // interleaved L,R pairs
    buf.mix_down();
    CHECK(buf.num_channels == 1, "mono after mix");
    CHECK(buf.size() == 4, "frame count preserved");
    const float want[4] = {3.0f, 4.0f, 5.0f, 6.0f};
    for (int i = 0; i < 4; ++i)
        CHECK(near(buf.data[static_cast<size_t>(i)], want[i]), "per-frame mean");
}

static void test_mix_down_three_channels() {
    std::printf("[mix_down_three_channels]\n");
    AudioBuffer buf(48000, 3);
    // interleave [f0c0,f0c1,f0c2, f1c0,f1c1,f1c2]:
    // f0 = (0,3,6) mean 3.0; f1 = (9,12,15) mean 12.0
    buf.data = {0, 3, 6, 9, 12, 15};
    buf.mix_down();
    CHECK(buf.num_channels == 1, "mono");
    CHECK(buf.size() == 2, "two frames");
    CHECK(near(buf.data[0], 3.0f), "frame0 mean");
    CHECK(near(buf.data[1], 12.0f), "frame1 mean");
}

static void test_mix_down_mono_noop() {
    std::printf("[mix_down_mono_noop]\n");
    AudioBuffer buf(44100, 1);
    buf.data = {1.0f, 2.0f, 3.0f};
    buf.mix_down();
    CHECK(buf.num_channels == 1, "stays mono");
    CHECK(buf.size() == 3, "untouched");
    CHECK(near(buf.data[2], 3.0f), "value kept");
}

static void test_at_indexing() {
    std::printf("[at_indexing]\n");
    AudioBuffer buf(44100, 2);
    buf.data = {10, 20, 30, 40};
    CHECK(near(buf.at(0, 0), 10.0f), "ch0 f0");
    CHECK(near(buf.at(1, 0), 20.0f), "ch1 f0");
    CHECK(near(buf.at(0, 1), 30.0f), "ch0 f1");
    CHECK(near(buf.at(1, 1), 40.0f), "ch1 f1");
}

static void test_resample_size_exact() {
    std::printf("[resample_size_exact]\n");
    AudioBuffer buf(44100, 1);
    buf.data.assign(44100, 0.25f);
    buf.resample(48000);
    CHECK(buf.sample_rate == 48000, "rate updated");
    CHECK(buf.size() == 48000, "size = round(44100*48000/44100)");
    bool kept = true;
    for (float v : buf.data)
        if (!near(v, 0.25f, 1e-6f)) {
            kept = false;
            break;
        }
    CHECK(kept, "DC preserved through resample");
}

static void test_resample_down() {
    std::printf("[resample_down]\n");
    AudioBuffer buf(48000, 2);
    buf.data.assign(96000, 0.5f);  // 48000 frames stereo DC
    buf.resample(16000);
    CHECK(buf.sample_rate == 16000, "rate updated");
    CHECK(buf.size() == 16000, "size = round(48000*16000/48000)");
    CHECK(buf.num_channels == 2, "channels kept");
    CHECK(near(buf.at(0, 0), 0.5f, 1e-6f), "ch0 DC");
    CHECK(near(buf.at(1, 15999), 0.5f, 1e-6f), "ch1 DC tail");
}

static void test_resample_noop_cases() {
    std::printf("[resample_noop_cases]\n");
    AudioBuffer buf(44100, 1);
    buf.data = {1.0f, 2.0f};
    buf.resample(44100);
    CHECK(buf.size() == 2 && near(buf.data[0], 1.0f), "same rate noop");
    buf.resample(0);
    CHECK(buf.sample_rate == 44100, "invalid rate ignored");
    AudioBuffer zeros(44100, 1);  // ctor fills sr*nc zeros (not empty)
    zeros.data.assign(44100, 0.0f);
    zeros.resample(48000);
    CHECK(zeros.sample_rate == 48000 && zeros.size() == 48000, "zero-fill resamples cleanly");
    bool allz = true;
    for (float v : zeros.data)
        if (v != 0.0f) {
            allz = false;
            break;
        }
    CHECK(allz, "zeros stay zero");
}

static void test_resample_sine_duration() {
    std::printf("[resample_sine_duration]\n");
    // 1s 440Hz sine at 44100 -> 48000: duration preserved sample-wise,
    // zero crossings scale with the rate ratio (independent check).
    const int src_n = 44100;
    AudioBuffer buf(44100, 1);
    buf.data.resize(static_cast<size_t>(src_n));
    for (int i = 0; i < src_n; ++i)
        buf.data[static_cast<size_t>(i)] =
            std::sin(2.0f * 3.14159265f * 440.0f * i / 44100.0f);
    auto crossings = [](const std::vector<float>& d) {
        int c = 0;
        for (size_t i = 1; i < d.size(); ++i)
            if ((d[i - 1] < 0) != (d[i] < 0)) ++c;
        return c;
    };
    const int src_zc = crossings(buf.data);  // ~880 for 440Hz/1s
    buf.resample(48000);
    CHECK(buf.size() == 48000, "1s stays 1s");
    const int dst_zc = crossings(buf.data);
    CHECK(dst_zc >= src_zc - 4 && dst_zc <= src_zc + 4, "zero crossings preserved");
}

int main() {
    test_mix_down_exact();
    test_mix_down_three_channels();
    test_mix_down_mono_noop();
    test_at_indexing();
    test_resample_size_exact();
    test_resample_down();
    test_resample_noop_cases();
    test_resample_sine_duration();
    std::printf("\n=== audio_buffer: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
