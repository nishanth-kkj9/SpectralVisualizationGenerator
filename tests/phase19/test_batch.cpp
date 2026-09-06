// Phase 19 — Batch integration tests: folders, failure isolation,
// recursive scan, retry accounting, mirrored output naming.

#include "batch.h"

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

static void write_wav(const std::string& path, double freq_hz) {
    const int sr = 22050, n = sr;  // 1s
    std::ofstream f(path, std::ios::binary);
    int data_size = n * 2, chunk = 36 + data_size;
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
    for (int i = 0; i < n; ++i) {
        float s = static_cast<float>(std::sin(2 * 3.14159265 * freq_hz * i / sr));
        int16_t v = static_cast<int16_t>(s * 32767.0f);
        f.write(reinterpret_cast<const char*>(&v), 2);
    }
}

static Spectral::GenerateConfig base_cfg(const std::string& out) {
    Spectral::GenerateConfig cfg;
    cfg.output_path = out;  // replaced per file by run_batch
    cfg.visualization = "spectrogram";
    cfg.fft_size = 512;
    cfg.width = 256;
    cfg.height = 128;
    return cfg;
}

int main() {
    const std::string root = "phase19_batch_in";
    const std::string out = "phase19_batch_out";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(out, ec);
    fs::create_directories(root + "/sub", ec);
    write_wav(root + "/a.wav", 440.0);
    write_wav(root + "/b.wav", 880.0);
    write_wav(root + "/sub/c.wav", 660.0);
    { std::ofstream bad(root + "/bad.wav", std::ios::binary); bad << "not audio"; }

    std::atomic<bool> cancel{false};
    Spectral::BatchOptions opts;
    opts.jobs = 2;
    opts.retries = 1;

    // 1. Non-recursive: finds a, b, bad (not sub/c)
    auto files = Spectral::collect_inputs(root, opts);
    EXPECT(files.size() == 3, "non-recursive collects 3 files");

    // 2. Recursive: finds all 4
    opts.recursive = true;
    auto files_r = Spectral::collect_inputs(root, opts);
    EXPECT(files_r.size() == 4, "recursive collects 4 files");

    // 3. Run: failure isolation — good files ok, bad fails after 2 attempts
    opts.recursive = false;
    auto results = Spectral::run_batch(base_cfg(out), files, root, out, opts, cancel);
    int ok = 0;
    bool bad_isolated = false, mirrored = false;
    for (const auto& r : results) {
        if (r.ok) {
            ++ok;
            EXPECT(fs::exists(r.output), ("output exists: " + r.output).c_str());
            EXPECT(r.attempts == 1, "success on first attempt");
        } else if (r.input.find("bad.wav") != std::string::npos) {
            bad_isolated = true;
            EXPECT(r.attempts == 2, "bad file retried once (attempts=2)");
        }
    }
    EXPECT(ok == 2, "2 of 3 succeed");
    EXPECT(bad_isolated, "bad file isolated, others unaffected");

    // 4. Mirrored naming: recursive run places sub/c.png under out/sub/
    auto results_r = Spectral::run_batch(base_cfg(out), files_r, root, out, opts, cancel);
    for (const auto& r : results_r) {
        if (r.input.find("c.wav") != std::string::npos && r.ok) {
            mirrored = (r.output == (fs::path(out) / "sub" / "c.png").string());
        }
    }
    EXPECT(mirrored, "recursive output mirrors tree (out/sub/c.png)");

    // 5. Cancel: pre-cancelled flag yields no successes
    cancel.store(true);
    auto results_c = Spectral::run_batch(base_cfg(out), files, root, out, opts, cancel);
    bool none_ok = true;
    for (const auto& r : results_c) none_ok = none_ok && !r.ok;
    EXPECT(none_ok, "cancelled batch produces no successes");

    fs::remove_all(root, ec);
    fs::remove_all(out, ec);
    printf("\nPhase 19 batch: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
