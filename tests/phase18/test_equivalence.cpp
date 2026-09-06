// Phase 18 — CLI/GUI equivalence: same GenerateConfig through the shared
// pipeline must produce byte-identical output. CLI and GUI both call run_job,
// so this pins determinism of the shared path.

#include "pipeline.h"

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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("usage: test_equivalence <input.wav>\n");
        return 2;
    }
    int fails = 0;

    Spectral::GenerateConfig cfg;
    cfg.input_path = argv[1];
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
