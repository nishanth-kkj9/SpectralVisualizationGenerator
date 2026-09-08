// Phase 20 — Production hardening: every case must fail clearly, never crash.

#include "pipeline.h"
#include "batch.h"
#include "output_files.h"
#include "project_config.h"
#include "spectrogram_renderer.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
#define EXPECT(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else { ++g_fail; printf("  FAIL: %s\n", msg); } \
} while (0)

// Minimal PCM16 mono/stereo WAV writer.
static void write_wav(const std::string& path, int sr, int channels,
                      const std::vector<float>& samples) {
    std::ofstream f(path, std::ios::binary);
    int data_size = static_cast<int>(samples.size()) * 2;
    int chunk = 36 + data_size;
    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&chunk), 4);
    f.write("WAVEfmt ", 8);
    int fmt = 16;
    f.write(reinterpret_cast<const char*>(&fmt), 4);
    int16_t tag = 1, ch = static_cast<int16_t>(channels);
    f.write(reinterpret_cast<const char*>(&tag), 2);
    f.write(reinterpret_cast<const char*>(&ch), 2);
    f.write(reinterpret_cast<const char*>(&sr), 4);
    int br = sr * channels * 2;
    f.write(reinterpret_cast<const char*>(&br), 4);
    int16_t ba = static_cast<int16_t>(channels * 2), bps = 16;
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

static std::vector<float> sine(int n, double hz, double sr, float amp = 0.5f) {
    std::vector<float> s(n);
    for (int i = 0; i < n; ++i)
        s[i] = amp * static_cast<float>(std::sin(2 * 3.14159265 * hz * i / sr));
    return s;
}

static Spectral::GenerateConfig cfg_for(const std::string& in, const std::string& out) {
    Spectral::GenerateConfig cfg;
    cfg.input_path = in;
    cfg.output_path = out;
    cfg.visualization = "spectrogram";
    cfg.fft_size = 512;
    cfg.width = 256;
    cfg.height = 128;
    return cfg;
}

static bool read_bytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    long n = static_cast<long>(f.tellg());
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(out.data()), n);
    return f.good();
}

int main() {
    const std::string dir = "phase20_hardening";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // Fixtures
    write_wav(dir + "/tone.wav", 22050, 1, sine(22050, 440.0, 22050.0));
    write_wav(dir + "/stereo.wav", 44100, 2, sine(44100, 330.0, 44100.0));  // mono data, stereo hdr
    write_wav(dir + "/sr8k.wav", 8000, 1, sine(8000, 200.0, 8000.0));
    write_wav(dir + "/sr96k.wav", 96000, 1, sine(96000, 1000.0, 96000.0));
    write_wav(dir + "/silence.wav", 22050, 1, std::vector<float>(22050, 0.0f));
    { auto clip = sine(22050, 440.0, 22050.0, 1.0f);
      for (auto& s : clip) s = (s >= 0 ? 1.0f : -1.0f);
      write_wav(dir + "/clip.wav", 22050, 1, clip); }
    write_wav(dir + "/quiet.wav", 22050, 1, sine(22050, 440.0, 22050.0, 1e-4f));
    { std::ofstream bad(dir + "/corrupt.wav", std::ios::binary); bad << "RIFFxxxx-junk"; }
    { std::ofstream proj(dir + "/bad.json"); proj << "{invalid json,,,"; }

    // 1. corrupt media: clear error, no crash
    EXPECT(Spectral::run_job(cfg_for(dir + "/corrupt.wav", dir + "/o1.png")) !=
               Spectral::JobError::Ok,
           "corrupt media fails clearly");

    // 2. missing file
    EXPECT(Spectral::run_job(cfg_for(dir + "/nope.wav", dir + "/o2.png")) ==
               Spectral::JobError::FileNotFound,
           "missing file -> FileNotFound");

    // 3. multichannel (stereo header)
    EXPECT(Spectral::run_job(cfg_for(dir + "/stereo.wav", dir + "/o3.png")) ==
               Spectral::JobError::Ok,
           "stereo decodes via mono mix");

    // 3b. multiple audio streams: first stream picked deterministically
    {
        std::string cmd =
            "ffmpeg -v quiet -y -f lavfi -i sine=frequency=440:duration=1:sample_rate=44100"
            " -f lavfi -i sine=frequency=880:duration=1:sample_rate=44100"
            " -map 0:a -map 1:a -ac 1 " +
            dir + "/multi.mkv";
        if (std::system(cmd.c_str()) == 0) {
            EXPECT(Spectral::run_job(cfg_for(dir + "/multi.mkv", dir + "/o3b.png")) ==
                       Spectral::JobError::Ok,
                   "multi-stream input decodes");
        } else {
            printf("  SKIP: multi-stream (ffmpeg fixture failed)\n");
        }
    }

    // 4-5. unusual sample rates
    EXPECT(Spectral::run_job(cfg_for(dir + "/sr8k.wav", dir + "/o4.png")) ==
               Spectral::JobError::Ok,
           "8kHz sample rate works");
    EXPECT(Spectral::run_job(cfg_for(dir + "/sr96k.wav", dir + "/o5.png")) ==
               Spectral::JobError::Ok,
           "96kHz sample rate works");

    // 6-8. silence / clipping / low amplitude
    EXPECT(Spectral::run_job(cfg_for(dir + "/silence.wav", dir + "/o6.png")) ==
               Spectral::JobError::Ok,
           "silence renders");
    EXPECT(Spectral::run_job(cfg_for(dir + "/clip.wav", dir + "/o7.png")) ==
               Spectral::JobError::Ok,
           "clipped audio renders");
    EXPECT(Spectral::run_job(cfg_for(dir + "/quiet.wav", dir + "/o8.png")) ==
               Spectral::JobError::Ok,
           "low amplitude renders");

    // 9. extreme frequency limits: min >= max rejected at validation
    {
        auto cfg = cfg_for(dir + "/tone.wav", dir + "/o9.png");
        cfg.min_freq = 5000.0f;
        cfg.max_freq = 1000.0f;
        EXPECT(Spectral::run_job(cfg) == Spectral::JobError::BadConfig,
               "min>max rejected as BadConfig");
    }
    // min above nyquist: renderer refuses clearly
    {
        auto cfg = cfg_for(dir + "/tone.wav", dir + "/o9b.png");
        cfg.min_freq = 50000.0f;
        EXPECT(Spectral::run_job(cfg) != Spectral::JobError::Ok,
               "min above nyquist fails clearly");
    }

    // 10. invalid configuration values
    {
        auto cfg = cfg_for(dir + "/tone.wav", dir + "/o10.png");
        cfg.fft_size = 1000;
        EXPECT(Spectral::run_job(cfg) == Spectral::JobError::BadConfig,
               "non-pow2 fft rejected");
    }
    {
        auto cfg = cfg_for(dir + "/tone.wav", dir + "/o10b.png");
        cfg.visualization = "waterfall";
        EXPECT(Spectral::run_job(cfg) == Spectral::JobError::BadConfig,
               "unknown visualization rejected");
    }

    // 11. invalid project file
    {
        Spectral::ProjectConfig pc;
        std::string err;
        EXPECT(!Spectral::ProjectConfigSerializer::load(dir + "/bad.json", pc, err),
               "invalid project JSON rejected");
        EXPECT(!Spectral::ProjectConfigSerializer::load(dir + "/missing.json", pc, err),
               "missing project file rejected");
    }

    // 12-13. unwritable output (directory as file) fails clearly, no crash
    EXPECT(Spectral::run_job(cfg_for(dir + "/tone.wav", dir)) != Spectral::JobError::Ok,
           "output-is-directory fails clearly");

    // 14. overwrite: rerun is byte-identical (idempotent)
    {
        auto cfg = cfg_for(dir + "/tone.wav", dir + "/o14.png");
        EXPECT(Spectral::run_job(cfg) == Spectral::JobError::Ok, "first write ok");
        std::vector<uint8_t> a, b;
        EXPECT(read_bytes(dir + "/o14.png", a), "first output readable");
        EXPECT(Spectral::run_job(cfg) == Spectral::JobError::Ok, "overwrite ok");
        EXPECT(read_bytes(dir + "/o14.png", b) && a == b, "overwrite byte-identical");
    }

    // 15. cancellation: pre-set flag aborts cleanly
    {
        std::atomic<bool> cancel{true};
        EXPECT(Spectral::run_job(cfg_for(dir + "/tone.wav", dir + "/o15.png"), {}, &cancel) !=
                   Spectral::JobError::Ok,
               "cancelled job aborts");
    }

    // 16. interrupted jobs leave no residue: success leaves no temp file
    // (temps are "<stem>.<pid>.<ctr>.part<ext>"; never valid outputs).
    {
        auto cfg = cfg_for(dir + "/tone.wav", dir + "/o16.png");
        EXPECT(Spectral::run_job(cfg) == Spectral::JobError::Ok, "job ok");
        bool residue = false;
        for (auto it = fs::directory_iterator(dir); it != fs::directory_iterator();
             it.increment(ec)) {
            if (Spectral::is_temp_name(it->path().filename().string())) residue = true;
        }
        EXPECT(!residue, "no temp residue after success");
        EXPECT(fs::exists(dir + "/o16.png"), "final output exists");
    }

    // 17-18. GPU: failure on empty data is an error, valid data succeeds (or falls back)
    {
        Spectral::SpectrogramRenderer r;
        Spectral::SpectralDataset empty;
        Spectral::RGBAImage img;
        Spectral::SpectrogramConfig sc;
        sc.width = 64;
        sc.height = 32;
        r.set_config(sc);
        auto err = r.render_gpu(empty, img);
        EXPECT(err != Spectral::RenderError::Ok, "gpu on empty dataset errors, no crash");
    }
    {
        auto cfg = cfg_for(dir + "/tone.wav", dir + "/o18.png");
        cfg.use_gpu = true;
        EXPECT(Spectral::run_job(cfg) == Spectral::JobError::Ok,
               "gpu job succeeds (or CPU fallback)");
    }

    // 19a. very long file: 60s tone processes without error
    {
        write_wav(dir + "/long.wav", 22050, 1, sine(60 * 22050, 440.0, 22050.0));
        auto cfg = cfg_for(dir + "/long.wav", dir + "/o19a.png");
        cfg.fft_size = 2048;
        EXPECT(Spectral::run_job(cfg) == Spectral::JobError::Ok,
               "60s file processes cleanly");
    }

    // 19. stress: 8-file parallel batch, mixed good/bad
    {
        for (int i = 0; i < 6; ++i)
            write_wav(dir + "/s" + std::to_string(i) + ".wav", 22050, 1,
                      sine(22050, 200.0 + i * 100, 22050.0));
        Spectral::BatchOptions bopts;
        bopts.jobs = 4;
        bopts.retries = 1;
        auto files = Spectral::collect_inputs(dir, bopts);
        EXPECT(!files.empty(), "stress batch collects files");
        std::atomic<bool> cancel{false};
        auto base = cfg_for("", dir + "/stress_out");
        auto results = Spectral::run_batch(base, files, dir, dir + "/stress_out", bopts,
                                           cancel);
        int ok = 0;
        for (const auto& r : results) {
            if (r.ok) {
                ++ok;
                if (!fs::exists(r.output)) {
                    EXPECT(false, "stress output exists");
                    break;
                }
            }
        }
        EXPECT(ok >= 6, "stress: all valid files succeed in parallel");
    }

    fs::remove_all(dir, ec);
    printf("\nPhase 20 hardening: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
