// Phase 18 — CLI/GUI equivalence: same GenerateConfig through the shared
// pipeline must produce byte-identical output. CLI and GUI both call run_job,
// so this pins determinism of the shared path.

#include "pipeline.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(n));
    size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size();
}

// Minimal sine writer so CTest can run this with no fixture present.
// When argv[1] names a real file it is used unchanged (same contract).
static bool write_sine_wav(const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const int sr = 44100, n = sr;
    const uint32_t data_bytes = static_cast<uint32_t>(n * 2);
    const uint32_t riff = 36 + data_bytes;
    const uint32_t fmt_len = 16;
    const uint16_t tag = 1, nch = 1, ba = 2, bps = 16;
    const uint32_t rate = sr, br = sr * 2;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f);
    fwrite(&tag, 2, 1, f);
    fwrite(&nch, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
    for (int i = 0; i < n; ++i) {
        const int16_t s =
            static_cast<int16_t>(16000.0 * std::sin(2.0 * 3.14159265 * 440.0 * i / sr));
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("usage: test_equivalence <input.wav>\n");
        return 2;
    }
    int fails = 0;
    std::string input = argv[1];
    {
        FILE* probe = nullptr;
        const bool exists =
            (fopen_s(&probe, input.c_str(), "rb") == 0 && probe != nullptr);
        if (probe) fclose(probe);
        if (!exists) {
            input = "eq_input.wav";
            if (!write_sine_wav(input)) {
                printf("FAIL: cannot create fallback input\n");
                return 1;
            }
            printf("note: using generated fallback input %s\n", input.c_str());
        }
    }

    Spectral::GenerateConfig cfg;
    cfg.input_path = input;
    cfg.output_path = "phase18_run_a.png";
    cfg.visualization = "spectrogram";
    cfg.fft_size = 1024;
    cfg.width = 512;
    cfg.height = 256;

    if (Spectral::run_job(cfg) != Spectral::JobError::Ok) {
        printf("FAIL: run A failed\n");
        return 1;
    }
    cfg.output_path = "phase18_run_b.png";
    if (Spectral::run_job(cfg) != Spectral::JobError::Ok) {
        printf("FAIL: run B failed\n");
        return 1;
    }

    std::vector<uint8_t> a, b;
    if (!read_file("phase18_run_a.png", a) || !read_file("phase18_run_b.png", b)) {
        printf("FAIL: cannot read outputs\n");
        return 1;
    }
    if (a != b) {
        printf("FAIL: outputs differ (%zu vs %zu bytes)\n", a.size(), b.size());
        ++fails;
    } else {
        printf("PASS: identical outputs (%zu bytes)\n", a.size());
    }

    // validate_config rejects bad configs (GUI relies on this pre-flight)
    Spectral::GenerateConfig bad;
    if (Spectral::validate_config(bad).empty()) {
        printf("FAIL: empty config accepted\n");
        ++fails;
    } else {
        printf("PASS: empty config rejected\n");
    }
    bad = cfg;
    bad.fft_size = 1000;
    if (Spectral::validate_config(bad).empty()) {
        printf("FAIL: non-pow2 fft accepted\n");
        ++fails;
    } else {
        printf("PASS: non-pow2 fft rejected\n");
    }

    printf("\nPhase 18 equivalence: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails;
}
