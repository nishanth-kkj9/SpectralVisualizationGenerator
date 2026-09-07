// Phase 2 — Typed error classification: every failure class gets its own
// JobError code plus a diagnostic message. Conventions follow
// test_hardening.cpp (EXPECT macro, local fixtures, temp dir cleanup).
#include "pipeline.h"
#include "batch.h"
#include "media_decoder.h"
#include "project_config.h"
#include "process/safe_process.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Spectral::JobError;

static int g_fail = 0;
static int g_skip = 0;
#define EXPECT(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else { ++g_fail; printf("  FAIL: %s\n", msg); } \
} while (0)
#define SKIP(msg) do { \
    ++g_skip; printf("  SKIP: %s\n", msg); \
} while (0)

// Minimal PCM16 mono WAV writer (independent, obvious headers).
static void write_wav(const std::string& path, int sr,
                      const std::vector<float>& samples) {
    std::ofstream f(path, std::ios::binary);
    int data_size = static_cast<int>(samples.size()) * 2;
    int chunk = 36 + data_size;
    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&chunk), 4);
    f.write("WAVEfmt ", 8);
    int fmt = 16;
    f.write(reinterpret_cast<const char*>(&fmt), 4);
    int16_t tag = 1, ch = 1;
    f.write(reinterpret_cast<const char*>(&tag), 2);
    f.write(reinterpret_cast<const char*>(&ch), 2);
    f.write(reinterpret_cast<const char*>(&sr), 4);
    int br = sr * 2;
    f.write(reinterpret_cast<const char*>(&br), 4);
    int16_t ba = 2, bps = 16;
    f.write(reinterpret_cast<const char*>(&ba), 2);
    f.write(reinterpret_cast<const char*>(&bps), 2);
    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&data_size), 4);
    for (float s : samples) {
        float c = std::max(-1.0f, std::min(1.0f, s));
        int16_t v = static_cast<int16_t>(c * 32767.0f);
        f.write(reinterpret_cast<const char*>(&v), 2);
    }
}

// Minimal 8x8 24-bit BMP: ffprobe sees a video stream, no audio.
static void write_bmp(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    const int w = 8, h = 8;
    const int row = ((w * 3 + 3) / 4) * 4;
    const int img = row * h;
    const int total = 54 + img;
    f.write("BM", 2);
    f.write(reinterpret_cast<const char*>(&total), 4);
    const int zero = 0;
    f.write(reinterpret_cast<const char*>(&zero), 4);
    const int off = 54;
    f.write(reinterpret_cast<const char*>(&off), 4);
    const int hdr = 40;
    f.write(reinterpret_cast<const char*>(&hdr), 4);
    f.write(reinterpret_cast<const char*>(&w), 4);
    f.write(reinterpret_cast<const char*>(&h), 4);
    const int16_t planes = 1, bpp = 24;
    f.write(reinterpret_cast<const char*>(&planes), 2);
    f.write(reinterpret_cast<const char*>(&bpp), 2);
    const int comp = 0;
    f.write(reinterpret_cast<const char*>(&comp), 4);
    f.write(reinterpret_cast<const char*>(&img), 4);
    f.write(reinterpret_cast<const char*>(&zero), 4);
    f.write(reinterpret_cast<const char*>(&zero), 4);
    f.write(reinterpret_cast<const char*>(&zero), 4);
    f.write(reinterpret_cast<const char*>(&zero), 4);
    std::vector<char> px(static_cast<size_t>(img), 0);
    f.write(px.data(), static_cast<std::streamsize>(px.size()));
}

static std::vector<float> sine(int n, double hz, double sr, float amp = 0.5f) {
    std::vector<float> s(n);
    for (int i = 0; i < n; ++i)
        s[i] = amp * static_cast<float>(std::sin(2 * 3.14159265 * hz * i / sr));
    return s;
}

static Spectral::GenerateConfig cfg_for(const std::string& in,
                                        const std::string& out) {
    Spectral::GenerateConfig cfg;
    cfg.input_path = in;
    cfg.output_path = out;
    cfg.visualization = "spectrogram";
    cfg.fft_size = 512;
    cfg.width = 256;
    cfg.height = 128;
    return cfg;
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// True when real ffmpeg+ffprobe answer on PATH (no overrides active).
static bool have_tools() {
    Spectral::SafeProcess p;
    Spectral::SafeProcess::Options o;
    o.capture_stdout = true;
    if (!p.spawn(Spectral::resolve_tool("ffmpeg"), {"-version"}, o)) return false;
    if (p.wait() != 0) return false;
    if (!p.spawn(Spectral::resolve_tool("ffprobe"), {"-version"}, o)) return false;
    return p.wait() == 0;
}

int main() {
    using Spectral::JobError;
    const std::string dir = "phase_errclass";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // 9. Invalid configuration -> BadConfig (no tools needed; validated first).
    {
        auto cfg = cfg_for(dir + "/nothing.wav", dir + "/o9.png");
        cfg.fft_size = 999;
        Spectral::Error err = Spectral::run_job(cfg);
        EXPECT(err.code == JobError::BadConfig, "bad fft -> BadConfig");
        EXPECT(contains(err.message, "fft"), "bad fft message names fft");
        EXPECT(!fs::exists(dir + "/o9.png"), "no output on BadConfig");
    }

    // 1. Missing input file -> FileNotFound with the path in the message.
    {
        Spectral::Error err = Spectral::run_job(cfg_for(dir + "/nope.wav", dir + "/o1.png"));
        EXPECT(err.code == JobError::FileNotFound, "missing input -> FileNotFound");
        EXPECT(contains(err.message, "nope.wav"), "message carries the path");
        ::MediaDecoder dec;
        EXPECT(!dec.open(dir + "/nope.wav"), "decoder open fails");
        EXPECT(dec.open_status() == ::OpenStatus::MissingInput, "status is MissingInput");
    }

    // 2. Missing ffprobe executable -> DependencyMissing (not FileNotFound).
    {
        write_wav(dir + "/tone.wav", 22050, sine(22050, 440.0, 22050.0));
        Spectral::set_ffprobe_path("Z:/definitely/not/here/ffprobe.exe");
        ::MediaDecoder dec;
        EXPECT(!dec.open(dir + "/tone.wav"), "decoder open fails");
        EXPECT(dec.open_status() == ::OpenStatus::ToolMissing, "status is ToolMissing");
        Spectral::Error err = Spectral::run_job(cfg_for(dir + "/tone.wav", dir + "/o2.png"));
        Spectral::set_ffprobe_path("");
        EXPECT(err.code == JobError::DependencyMissing, "missing ffprobe -> DependencyMissing");
        EXPECT(contains(err.message, "ffprobe"), "message names ffprobe");
    }

    if (!have_tools()) {
        SKIP("ffmpeg/ffprobe absent: tool-dependent cases need real tools");
        fs::remove_all(dir, ec);
        printf("\n=== error_classification: %s ===\n", g_fail == 0 ? "PASS" : "FAIL");
        return g_fail == 0 ? 0 : 1;
    }

    // 3. Missing ffmpeg executable -> DependencyMissing (probe succeeds first).
    {
        Spectral::set_ffmpeg_path("Z:/definitely/not/here/ffmpeg.exe");
        Spectral::Error err = Spectral::run_job(cfg_for(dir + "/tone.wav", dir + "/o3.png"));
        Spectral::set_ffmpeg_path("");
        EXPECT(err.code == JobError::DependencyMissing, "missing ffmpeg -> DependencyMissing");
        EXPECT(contains(err.message, "ffmpeg"), "message names ffmpeg");
    }

    // 4. ffprobe starts but rejects the file -> ProbeFailed.
    {
        { std::ofstream bad(dir + "/corrupt.wav", std::ios::binary); bad << "RIFFxxxx-junk"; }
        Spectral::Error err = Spectral::run_job(cfg_for(dir + "/corrupt.wav", dir + "/o4.png"));
        EXPECT(err.code == JobError::ProbeFailed, "corrupt media -> ProbeFailed");
        EXPECT(contains(err.message, "ffprobe"), "message names ffprobe");
    }

    // 5. Valid media, no audio stream -> NoAudioStream.
    {
        write_bmp(dir + "/noaudio.bmp");
        Spectral::Error err = Spectral::run_job(cfg_for(dir + "/noaudio.bmp", dir + "/o5.png"));
        EXPECT(err.code == JobError::NoAudioStream, "video-only file -> NoAudioStream");
        EXPECT(contains(err.message, "no audio"), "message says no audio");
    }

    // 6a. Decoder starts but ffmpeg exits nonzero mid-stream -> failed().
    // Absurd resample rate fails resampler init deterministically.
    {
        write_wav(dir + "/full.wav", 22050, sine(22050, 440.0, 22050.0));
        ::MediaDecoder dec;
        ::MediaDecoder::DecodeOptions opts;
        opts.target_rate = 2147483647;
        EXPECT(dec.open(dir + "/full.wav", opts), "decode process starts");
        ::AudioFrame fr;
        while (dec.read_frame(fr)) {}
        EXPECT(dec.failed(), "nonzero ffmpeg exit flags failure");
        EXPECT(contains(dec.last_error(), "exited"), "diagnostic names the exit");
    }
    // 6b. Empty (0-sample) audio decodes cleanly to nothing -> DecodeError.
    {
        write_wav(dir + "/empty.wav", 22050, std::vector<float>());
        Spectral::Error err = Spectral::run_job(cfg_for(dir + "/empty.wav", dir + "/o6.png"));
        EXPECT(err.code == JobError::DecodeError, "empty audio -> DecodeError");
        EXPECT(contains(err.message, "0 audio samples"), "message says why");
    }

    // 7. Image rendering failure -> RenderError (empty dataset, no decode).
    {
        auto cfg = cfg_for(dir + "/tone.wav", dir + "/o7.png");
        Spectral::SpectralDataset empty;
        Spectral::Error err = Spectral::render_dataset(cfg, empty);
        EXPECT(err.code == JobError::RenderError, "empty dataset render -> RenderError");
    }

    // 8. Video encoding failure -> EncodeError (unknown codec, real ffmpeg).
    {
        auto cfg = cfg_for(dir + "/tone.wav", dir + "/o8.mp4");
        cfg.output_format = "video";
        cfg.video_codec = "no_such_codec_xyz";
        Spectral::Error err = Spectral::run_job(cfg);
        EXPECT(err.code == JobError::EncodeError, "bad codec -> EncodeError");
    }

    // 10. Successful generation -> Ok.
    {
        Spectral::Error err = Spectral::run_job(cfg_for(dir + "/tone.wav", dir + "/o10.png"));
        EXPECT(err.code == JobError::Ok, "valid job -> Ok");
        EXPECT(fs::exists(dir + "/o10.png"), "output produced");
    }

    fs::remove_all(dir, ec);
    printf("\n=== error_classification: %s (%d skipped) ===\n",
           g_fail == 0 ? "PASS" : "FAIL", g_skip);
    return g_fail == 0 ? 0 : 1;
}
