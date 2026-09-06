// tests/media/test_media_decoder.cpp
// S3 decoder correctness: streaming fixtures with independently known
// sample values, stream selection, failure behavior, hostile paths,
// chunking/memory bounds, timestamps, and tool resolution.
// Needs ffmpeg/ffprobe on PATH; otherwise prints SKIP and passes 0.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "media_decoder.h"
#include "process/safe_process.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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

static bool close_to(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

// ---------------------------------------------------------------------------
// Independent fixture writers (plain WAV; obvious headers, no cleverness).
// ---------------------------------------------------------------------------

static void write_wav_pcm16(const std::string& path, int sr, int channels,
                            const std::vector<std::vector<float>>& ch) {
    const size_t n = ch[0].size();
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "wb");
    const uint32_t data_bytes = static_cast<uint32_t>(n * channels * 2);
    const uint32_t riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    const uint32_t fmt_len = 16;
    fwrite(&fmt_len, 4, 1, f);
    const uint16_t tag = 1;
    fwrite(&tag, 2, 1, f);
    const uint16_t nch = static_cast<uint16_t>(channels);
    fwrite(&nch, 2, 1, f);
    const uint32_t rate = static_cast<uint32_t>(sr);
    fwrite(&rate, 4, 1, f);
    const uint32_t br = rate * channels * 2;
    fwrite(&br, 4, 1, f);
    const uint16_t ba = static_cast<uint16_t>(channels * 2);
    fwrite(&ba, 2, 1, f);
    const uint16_t bps = 16;
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < channels; ++c) {
            float v = ch[static_cast<size_t>(c)][i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            int16_t s = static_cast<int16_t>(std::lround(v * 32767.0f));
            fwrite(&s, 2, 1, f);
        }
    fclose(f);
}

static void write_wav_float32(const std::string& path, int sr,
                              const std::vector<float>& mono) {
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "wb");
    const uint32_t data_bytes = static_cast<uint32_t>(mono.size() * 4);
    const uint32_t riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    const uint32_t fmt_len = 16;
    fwrite(&fmt_len, 4, 1, f);
    const uint16_t tag = 3;
    fwrite(&tag, 2, 1, f);
    const uint16_t nch = 1;
    fwrite(&nch, 2, 1, f);
    const uint32_t rate = static_cast<uint32_t>(sr);
    fwrite(&rate, 4, 1, f);
    const uint32_t br = rate * 4;
    fwrite(&br, 4, 1, f);
    const uint16_t ba = 4;
    fwrite(&ba, 2, 1, f);
    const uint16_t bps = 32;
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
    fwrite(mono.data(), 4, mono.size(), f);
    fclose(f);
}

// Independent tone check (Goertzel): no production DSP involved.
static float goertzel(const std::vector<float>& x, int sr, float freq) {
    const double w = 2.0 * 3.14159265358979 * freq / sr;
    const double c = std::cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (float v : x) {
        s0 = v + 2 * c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return static_cast<float>(std::sqrt(s1 * s1 + s2 * s2 - 2 * c * s1 * s2) / x.size());
}

static bool have_tools() {
    Spectral::SafeProcess p;
    Spectral::SafeProcess::Options o;
    o.capture_stdout = true;
    if (!p.spawn(Spectral::resolve_tool("ffmpeg"), {"-version"}, o)) return false;
    p.wait();
    if (p.exit_code() != 0) return false;
    if (!p.spawn(Spectral::resolve_tool("ffprobe"), {"-version"}, o)) return false;
    p.wait();
    return p.exit_code() == 0;
}

static bool run_tool(const std::string& exe, const std::vector<std::string>& args) {
    Spectral::SafeProcess p;
    Spectral::SafeProcess::Options o;
    o.capture_stdout = true;
    o.capture_stderr = true;
    if (!p.spawn(exe, args, o)) return false;
    return p.wait() == 0;
}

static std::string tmpdir() {
    std::string d = "s3_media_tmp";
    fs::create_directories(d);
    return d;
}

// Drain helper: returns all samples + per-chunk frame counts + timestamps.
struct Drain {
    std::vector<float> all;
    std::vector<size_t> chunk_frames;
    std::vector<double> stamps;
    int sr = 0;
    int ch = 0;
    size_t max_chunk_samples = 0;
};

static bool drain_all(MediaDecoder& dec, Drain& d) {
    AudioFrame f;
    d.sr = dec.sample_rate();
    d.ch = dec.num_channels();
    while (dec.read_frame(f)) {
        d.chunk_frames.push_back(f.samples.size() / static_cast<size_t>(d.ch));
        d.stamps.push_back(f.timestamp);
        if (f.samples.size() > d.max_chunk_samples) d.max_chunk_samples = f.samples.size();
        d.all.insert(d.all.end(), f.samples.begin(), f.samples.end());
    }
    return !dec.failed();
}

// ---------------------------------------------------------------------------
// Fixture A — mono ramp, exact values.
// ---------------------------------------------------------------------------
static void test_mono_ramp(const std::string& dir) {
    std::printf("[mono_ramp]\n");
    const int sr = 44100, n = 5000;
    std::vector<float> ramp(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) ramp[static_cast<size_t>(i)] = (i % 1000) / 1000.0f;
    write_wav_pcm16(dir + "/a_mono.wav", sr, 1, {ramp});
    MediaDecoder dec;
    CHECK(dec.open(dir + "/a_mono.wav"), "open mono");
    CHECK(dec.sample_rate() == sr, "sample rate");
    CHECK(dec.num_channels() == 1, "mono channels");
    CHECK(dec.stream_index() == 0, "stream index 0");
    CHECK(!dec.codec_name().empty(), "codec named");
    CHECK(dec.duration_known(), "duration known");
    CHECK(dec.total_frames_known() == false, "count estimate before EOF");
    Drain d;
    CHECK(drain_all(dec, d), "clean decode");
    CHECK(d.all.size() == ramp.size(), "exact sample count");
    bool vals = d.all.size() == ramp.size();
    for (size_t i = 0; vals && i < ramp.size(); ++i) {
        float want = std::round(ramp[i] * 32767.0f) / 32768.0f;
        if (!close_to(d.all[i], want, 2e-4f)) vals = false;
    }
    CHECK(vals, "sample values match independently known ramp");
    CHECK(dec.total_frames_known(), "count exact after EOF");
    CHECK(dec.total_frames() == static_cast<int64_t>(n), "total == delivered");
    CHECK(dec.failed() == false, "not failed");
}

// ---------------------------------------------------------------------------
// Fixture B — stereo with deliberately different L/R.
// ---------------------------------------------------------------------------
static void test_stereo_distinct(const std::string& dir) {
    std::printf("[stereo_distinct]\n");
    const int sr = 48000, n = 3000;
    std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        L[static_cast<size_t>(i)] = (i % 500) / 500.0f;                      // ramp
        R[static_cast<size_t>(i)] = std::sin(2.0f * 3.14159f * 330.0f * i / sr);  // tone
    }
    write_wav_pcm16(dir + "/b_stereo.wav", sr, 2, {L, R});
    MediaDecoder dec;
    CHECK(dec.open(dir + "/b_stereo.wav"), "open stereo");
    CHECK(dec.num_channels() == 2, "stereo kept native (no -ac forcing)");
    Drain d;
    CHECK(drain_all(dec, d), "clean decode");
    CHECK(d.all.size() == static_cast<size_t>(2 * n), "frame count");
    bool ok = d.all.size() == static_cast<size_t>(2 * n);
    for (int i = 0; ok && i < n; ++i) {
        float wl = std::round(L[static_cast<size_t>(i)] * 32767.0f) / 32768.0f;
        float wr = std::round(R[static_cast<size_t>(i)] * 32767.0f) / 32768.0f;
        if (!close_to(d.all[static_cast<size_t>(2 * i)], wl, 2e-4f)) ok = false;
        if (!close_to(d.all[static_cast<size_t>(2 * i + 1)], wr, 2e-4f)) ok = false;
    }
    CHECK(ok, "L/R values land on correct channels (interleave intact)");
}

// ---------------------------------------------------------------------------
// Fixture C — 6 channels, distinct DC each.
// ---------------------------------------------------------------------------
static void test_multichannel(const std::string& dir) {
    std::printf("[multichannel]\n");
    const int sr = 44100, n = 1200;
    std::vector<std::vector<float>> chs;
    for (int c = 0; c < 6; ++c)
        chs.push_back(std::vector<float>(static_cast<size_t>(n), 0.1f * (c + 1)));
    write_wav_pcm16(dir + "/c_6ch.wav", sr, 6, chs);
    MediaDecoder dec;
    CHECK(dec.open(dir + "/c_6ch.wav"), "open 6ch");
    CHECK(dec.num_channels() == 6, "6 channels exposed, not forced stereo");
    Drain d;
    CHECK(drain_all(dec, d), "clean decode");
    bool ok = d.all.size() == static_cast<size_t>(6 * n);
    for (int i = 0; ok && i < n; ++i)
        for (int c = 0; c < 6; ++c) {
            float want = std::round(0.1f * (c + 1) * 32767.0f) / 32768.0f;
            if (!close_to(d.all[static_cast<size_t>(i * 6 + c)], want, 2e-4f)) ok = false;
        }
    CHECK(ok, "per-channel values correct");
}

// ---------------------------------------------------------------------------
// Fixture D — known sine; float32 + odd rate inputs.
// ---------------------------------------------------------------------------
static void test_sine_and_float(const std::string& dir) {
    std::printf("[sine_and_float]\n");
    const int sr = 22050, n = 22050;  // 1s, odd rate
    std::vector<float> tone(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        tone[static_cast<size_t>(i)] = 0.7f * std::sin(2.0f * 3.14159f * 440.0f * i / sr);
    write_wav_float32(dir + "/d_tone_f32.wav", sr, tone);
    MediaDecoder dec;
    CHECK(dec.open(dir + "/d_tone_f32.wav"), "open float32");
    CHECK(dec.sample_rate() == sr, "odd rate preserved");
    Drain d;
    CHECK(drain_all(dec, d), "clean decode");
    CHECK(d.all.size() == tone.size(), "count");
    float g440 = goertzel(d.all, sr, 440.0f);
    float g900 = goertzel(d.all, sr, 900.0f);
    CHECK(g440 > 10.0f * g900, "440Hz dominates (independent Goertzel)");
}

// ---------------------------------------------------------------------------
// Fixture E — resample 44100 -> 48000 through the decoder.
// ---------------------------------------------------------------------------
static void test_decode_resample(const std::string& dir) {
    std::printf("[decode_resample]\n");
    const int sr = 44100, n = 44100;
    std::vector<float> tone(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        tone[static_cast<size_t>(i)] = 0.6f * std::sin(2.0f * 3.14159f * 440.0f * i / sr);
    write_wav_pcm16(dir + "/e_tone.wav", sr, 1, {tone});
    MediaDecoder dec;
    MediaDecoder::DecodeOptions opts;
    opts.target_rate = 48000;
    CHECK(dec.open(dir + "/e_tone.wav", opts), "open with target rate");
    CHECK(dec.sample_rate() == 48000, "effective rate is target");
    Drain d;
    CHECK(drain_all(dec, d), "clean decode");
    CHECK(d.all.size() == 48000, "duration preserved in samples");
    float g440 = goertzel(d.all, 48000, 440.0f);
    float g900 = goertzel(d.all, 48000, 900.0f);
    CHECK(g440 > 8.0f * g900, "tone survives resampling");
}

// ---------------------------------------------------------------------------
// Stream selection, video, failures.
// ---------------------------------------------------------------------------
static void test_multistream_select(const std::string& dir, const std::string& ff) {
    std::printf("[multistream_select]\n");
    const std::string mkv = dir + "/multi.mkv";
    bool made = run_tool(ff, {"-y", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=1",
                              "-f", "lavfi", "-i", "sine=frequency=880:duration=1",
                              "-map", "0:a", "-map", "1:a", "-c:a", "pcm_s16le", mkv});
    CHECK(made, "multistream fixture created");
    if (!made) return;
    MediaDecoder d1, d2;
    CHECK(d1.open(mkv), "open multistream");
    CHECK(d1.stream_index() == 0, "deterministic lowest-index selection");
    Drain a, b;
    CHECK(drain_all(d1, a), "decode stream 0");
    CHECK(d2.open(mkv), "reopen");
    CHECK(drain_all(d2, b), "decode again");
    CHECK(a.all == b.all, "selection deterministic across opens");
    float g440 = goertzel(a.all, d1.sample_rate(), 440.0f);
    float g880 = goertzel(a.all, d1.sample_rate(), 880.0f);
    CHECK(g440 > 5.0f * g880, "selected stream is the 440Hz one");
}

static void test_video_audio(const std::string& dir, const std::string& ff) {
    std::printf("[video_audio]\n");
    const std::string mp4 = dir + "/v_clip.mp4";
    bool made = run_tool(ff, {"-y", "-v", "error", "-f", "lavfi", "-i", "testsrc=duration=1:size=64x64:rate=5",
                              "-f", "lavfi", "-i", "sine=frequency=660:duration=1",
                              "-pix_fmt", "yuv420p", "-c:v", "libx264", "-c:a", "aac", mp4});
    if (!made) {
        std::printf("  SKIP: mp4 fixture needs libx264+aac\n");
        ++g_run;
        ++g_pass;
        return;
    }
    MediaDecoder dec;
    CHECK(dec.open(mp4), "open mp4");
    CHECK(dec.num_channels() >= 1, "audio found in video");
    Drain d;
    CHECK(drain_all(dec, d), "clean decode");
    CHECK(!d.all.empty(), "samples from video container");
    float g = goertzel(d.all, d.sr, 660.0f);
    float o = goertzel(d.all, d.sr, 990.0f);
    CHECK(g > 3.0f * o, "660Hz tone recovered from mp4");
}

static void test_failures(const std::string& dir) {
    std::printf("[failures]\n");
    {
        MediaDecoder dec;
        CHECK(!dec.open(dir + "/does_not_exist.wav"), "missing file fails");
        CHECK(!dec.last_error().empty(), "missing file has diagnostics");
    }
    {
        // fixed-seed garbage: deterministic corrupt input
        std::string p = dir + "/corrupt.bin";
        FILE* f = nullptr;
        fopen_s(&f, p.c_str(), "wb");
        uint32_t s = 12345;
        for (int i = 0; i < 512; ++i) {
            s = s * 1664525u + 1013904223u;
            fputc(static_cast<int>((s >> 16) & 0xFF), f);
        }
        fclose(f);
        MediaDecoder dec;
        bool ok = dec.open(p);
        // Either probe rejects or decode fails — but never a crash, and a
        // failed decode must be flagged, never silent success-with-garbage.
        AudioFrame fr;
        size_t got = 0;
        if (ok) {
            while (dec.read_frame(fr)) got += fr.samples.size();
        }
        CHECK(!ok || dec.failed() || got == 0, "corrupt media fails clearly");
    }
    {
        MediaDecoder dec;
        CHECK(!dec.open(dir + "/does_not_exist2.wav"), "missing fails");
        AudioFrame fr;
        CHECK(!dec.read_frame(fr), "read after failed open is false");
    }
    {
        // bad tool path: deterministic "ffmpeg unavailable" behavior
        Spectral::set_ffmpeg_path("Z:/definitely/not/here/ffmpeg.exe");
        Spectral::set_ffprobe_path("Z:/definitely/not/here/ffprobe.exe");
        MediaDecoder dec;
        CHECK(!dec.open(dir + "/a_mono.wav"), "missing tools fail open");
        CHECK(dec.last_error().find("cannot start") != std::string::npos ||
                  dec.last_error().find("ffprobe") != std::string::npos,
              "diagnostic names the tool failure");
        Spectral::set_ffmpeg_path("");
        Spectral::set_ffprobe_path("");
    }
}

static void test_truncated(const std::string& dir) {
    std::printf("[truncated]\n");
    const int sr = 44100, n = 20000;
    std::vector<float> tone(static_cast<size_t>(n), 0.3f);
    write_wav_pcm16(dir + "/full.wav", sr, 1, {tone});
    // cut the file at 60%: header intact, data short
    std::string cut = dir + "/cut.wav";
    {
        FILE* fi = nullptr;
        fopen_s(&fi, (dir + "/full.wav").c_str(), "rb");
        fseek(fi, 0, SEEK_END);
        long total = ftell(fi);
        fseek(fi, 0, SEEK_SET);
        std::vector<char> buf(static_cast<size_t>(total));
        size_t got = fread(buf.data(), 1, static_cast<size_t>(total), fi);
        fclose(fi);
        FILE* fo = nullptr;
        fopen_s(&fo, cut.c_str(), "wb");
        fwrite(buf.data(), 1, got * 6 / 10, fo);
        fclose(fo);
    }
    MediaDecoder dec;
    CHECK(dec.open(cut), "truncated header still probes");
    Drain d;
    drain_all(dec, d);  // must not crash; result flagged or short, never garbage
    CHECK(!d.all.empty(), "partial samples delivered");
    CHECK(d.all.size() < tone.size(), "short of the full count");
}

// ---------------------------------------------------------------------------
// Security: hostile filenames must decode byte-identically.
// ---------------------------------------------------------------------------
static void test_hostile_paths(const std::string& dir) {
    std::printf("[hostile_paths]\n");
    const std::vector<std::string> names = {
        "spa ce.wav", "a&b.wav", "semi;colon.wav", "quo'te.wav",
        "pct%20.wav", "excl!am.wav", "bra[ck]et.wav", "par(en).wav",
    };
    const int sr = 16000, n = 2000;
    std::vector<float> tone(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        tone[static_cast<size_t>(i)] = 0.4f * std::sin(2.0f * 3.14159f * 220.0f * i / sr);
    std::vector<float> reference;
    bool first = true;
    for (const auto& nm : names) {
        const std::string p = dir + "/" + nm;
        write_wav_pcm16(p, sr, 1, {tone});
        MediaDecoder dec;
        if (!dec.open(p)) {
            CHECK(false, ("open hostile: " + nm).c_str());
            continue;
        }
        Drain d;
        if (!drain_all(dec, d)) {
            CHECK(false, ("decode hostile: " + nm).c_str());
            continue;
        }
        if (first) {
            reference = d.all;
            first = false;
            CHECK(d.all.size() == tone.size(), "hostile baseline count");
        } else {
            CHECK(d.all == reference, ("identical bytes: " + nm).c_str());
        }
    }
    // NOTE: a literal double-quote cannot exist in a Windows filename, so
    // there is no quote-in-name file to test. Quoting is still required for
    // paths with spaces (covered above); with no shell involved, quotes in
    // *arguments* stay literal by construction (see quote_arg unit path).
}

// ---------------------------------------------------------------------------
// Chunking, timestamps, memory bounds.
// ---------------------------------------------------------------------------
static void test_chunking_and_time(const std::string& dir) {
    std::printf("[chunking_and_time]\n");
    const int sr = 44100;
    const int n = sr * 5;  // 5s >> 4096-frame chunks
    std::vector<float> ramp(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) ramp[static_cast<size_t>(i)] = (i % 1000) / 1000.0f;
    write_wav_pcm16(dir + "/long.wav", sr, 2, {ramp, ramp});
    MediaDecoder dec;
    MediaDecoder::DecodeOptions opts;
    opts.chunk_frames = 4096;
    CHECK(dec.open(dir + "/long.wav", opts), "open long");
    Drain d;
    CHECK(drain_all(dec, d), "clean multi-chunk decode");
    CHECK(d.all.size() == static_cast<size_t>(2 * n), "no loss, no dupes");
    CHECK(d.chunk_frames.size() > 10, "forced through many chunks");
    bool bounded = true, mono = true, ordered = true;
    for (size_t i = 0; i < d.chunk_frames.size(); ++i) {
        if (d.chunk_frames[i] > 4096) bounded = false;
        if (i > 0 && d.stamps[i] <= d.stamps[i - 1]) ordered = false;
    }
    CHECK(bounded, "every chunk within bound");
    CHECK(ordered, "timestamps strictly increase");
    CHECK(close_to(static_cast<float>(d.stamps[0]), 0.0f, 1e-3f), "first stamp ~0");
    double expect_last = static_cast<double>((d.chunk_frames.size() - 1) * 4096) / sr;
    CHECK(std::fabs(d.stamps.back() - expect_last) < 0.01, "stamp math exact");
    for (size_t i = 0; mono && i < static_cast<size_t>(n); ++i) {
        float want = std::round(ramp[i] * 32767.0f) / 32768.0f;
        if (!close_to(d.all[2 * i], want, 2e-4f) || !close_to(d.all[2 * i + 1], want, 2e-4f)) mono = false;
    }
    CHECK(mono, "values exact across chunk seams");
    CHECK(d.max_chunk_samples <= 4096u * 2u, "decoder never holds >1 chunk");
}

static void test_memory_flat(const std::string& dir) {
    std::printf("[memory_flat]\n");
    // 30s mono: decode-and-discard. Boundedness is proven by the chunk cap
    // (decoder state must not scale with duration); totals prove no loss.
    const int sr = 44100, n = sr * 30;
    std::vector<float> dc(static_cast<size_t>(n), 0.2f);
    write_wav_pcm16(dir + "/mem.wav", sr, 1, {dc});
    MediaDecoder dec;
    CHECK(dec.open(dir + "/mem.wav"), "open 30s");
    AudioFrame f;
    int64_t total = 0;
    size_t peak = 0;
    while (dec.read_frame(f)) {
        total += static_cast<int64_t>(f.samples.size());
        if (f.samples.size() > peak) peak = f.samples.size();
    }
    CHECK(!dec.failed(), "clean long decode");
    CHECK(total == n, "all samples, none lost/duplicated");
    CHECK(peak <= 4096, "peak live chunk bounded (flat memory)");
    std::printf("  info: 30s decoded, peak live samples=%zu\n", peak);
}

// ---------------------------------------------------------------------------
// Tool resolution precedence (no files needed).
// ---------------------------------------------------------------------------
static void test_quote_arg() {
    std::printf("[quote_arg]\n");
    CHECK(Spectral::quote_arg("abc") == "abc", "plain untouched");
    CHECK(Spectral::quote_arg("") == "\"\"", "empty quoted");
    CHECK(Spectral::quote_arg("a b&c;d|e") == "\"a b&c;d|e\"", "metachars wrapped");
    CHECK(Spectral::quote_arg("a\"b") == "\"a\\\"b\"", "embedded quote escaped");
    CHECK(Spectral::quote_arg("C:\\p\\") == "C:\\p\\", "trailing backslash untouched");
    // every hostile char must end up inside one quoted argv element
    const std::string hostile = "x &;|'\"$%![]()";
    const std::string q = Spectral::quote_arg(hostile);
    CHECK(q.size() >= hostile.size() + 2 && q.front() == '"' && q.back() == '"',
          "hostile fully wrapped");
}

static void test_tool_resolution() {
    std::printf("[tool_resolution]\n");
    Spectral::set_ffmpeg_path("X:/explicit/ffmpeg.exe");
    CHECK(Spectral::resolve_tool("ffmpeg") == "X:/explicit/ffmpeg.exe", "explicit wins");
    Spectral::set_ffmpeg_path("");
    _putenv_s("FFMPEG_BINARY", "Y:/env/ffmpeg.exe");
    CHECK(Spectral::resolve_tool("ffmpeg") == "Y:/env/ffmpeg.exe", "env second");
    _putenv_s("FFMPEG_BINARY", "");
    CHECK(Spectral::resolve_tool("ffmpeg") == "ffmpeg", "PATH fallback");
    Spectral::set_ffprobe_path("X:/explicit/ffprobe.exe");
    CHECK(Spectral::resolve_tool("ffprobe") == "X:/explicit/ffprobe.exe", "probe explicit");
    Spectral::set_ffprobe_path("");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // crash diagnostics must not sit in the buffer
    test_quote_arg();
    test_tool_resolution();
    if (!have_tools()) {
        std::printf("SKIP: ffmpeg/ffprobe not on PATH — media tests need them\n");
        return 0;
    }
    const std::string dir = tmpdir();
    test_mono_ramp(dir);
    test_stereo_distinct(dir);
    test_multichannel(dir);
    test_sine_and_float(dir);
    test_decode_resample(dir);
    const std::string ff = Spectral::resolve_tool("ffmpeg");
    test_multistream_select(dir, ff);
    test_video_audio(dir, ff);
    test_failures(dir);
    test_truncated(dir);
    test_hostile_paths(dir);
    test_chunking_and_time(dir);
    test_memory_flat(dir);
    std::printf("\n=== media_decoder: %d/%d passed ===\n", g_pass, g_run);
    fs::remove_all(dir);
    return (g_pass == g_run) ? 0 : 1;
}
