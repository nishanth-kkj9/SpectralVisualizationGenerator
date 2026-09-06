// tests/cli/test_reproducibility.cpp
// S5 end-to-end: wav -> canonical config -> dataset -> save -> load ->
// render x2 identical. Requires ffmpeg (decode); otherwise SKIP + pass.
#include "pipeline.h"
#include "process/safe_process.h"
#include "project_config.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

static bool write_sine_wav(const std::string& path, int sr, int n, float freq) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const uint32_t data_bytes = static_cast<uint32_t>(n * 2);
    const uint32_t riff = 36 + data_bytes;
    const uint32_t fmt_len = 16;
    const uint16_t tag = 1, nch = 1, ba = 2, bps = 16;
    const uint32_t rate = static_cast<uint32_t>(sr), br = rate * 2;
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
        const int16_t s = static_cast<int16_t>(
            15000.0 * std::sin(2.0 * 3.14159265 * freq * i / sr));
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return true;
}

static bool read_bytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static Spectral::GenerateConfig base_cfg(const std::string& dir) {
    Spectral::GenerateConfig cfg;
    cfg.input_path = dir + "/s5_tone.wav";
    cfg.output_path = dir + "/s5_out.png";
    cfg.visualization = "spectrogram";
    cfg.fft_size = 1024;
    cfg.hop_size = 512;
    cfg.window = "hann";
    cfg.width = 256;
    cfg.height = 128;
    return cfg;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // tool presence without decoding anything yet
    {
        Spectral::SafeProcess p;
        Spectral::SafeProcess::Options o;
        o.capture_stdout = true;
        if (!p.spawn(Spectral::resolve_tool("ffmpeg"), {"-version"}, o) || p.wait() != 0) {
            std::printf("SKIP: ffmpeg not on PATH\n");
            return 0;
        }
    }
    const std::string dir = "s5_repro_tmp";
    fs::create_directories(dir);
    CHECK(write_sine_wav(dir + "/s5_tone.wav", 44100, 44100, 440.0f), "fixture written");

    auto cfg = base_cfg(dir);
    // analyze twice: same config -> same dataset bytes + identity
    Spectral::SpectralDataset d1, d2;
    std::vector<float> s1, s2;
    CHECK(Spectral::analyze_dataset(cfg, d1, s1).ok(), "analyze run 1");
    CHECK(Spectral::analyze_dataset(cfg, d2, s2).ok(), "analyze run 2");
    std::vector<uint8_t> b1, b2;
    CHECK(d1.serialize_binary(b1), "serialize 1");
    CHECK(d2.serialize_binary(b2), "serialize 2");
    CHECK(b1 == b2, "same config -> same dataset bytes");
    CHECK(d1.dataset_identity() == d2.dataset_identity(), "identity stable");
    CHECK(d1.dataset_identity().size() == 64, "identity is sha256 hex");
    // canonical metadata came from the bridge, not ad-hoc locals
    CHECK(d1.source_metadata().file_hash.size() == 64, "source content hashed");
    CHECK(d1.analysis_metadata().analyzer_version == "2", "analysis version recorded");
    CHECK(d1.analysis_metadata().analysis_method == "stft", "method recorded");
    CHECK(d1.channel_info().channels_mixed == false, "mono not marked mixed");
    CHECK(d1.normalization_info().window_coherent_gain > 0.49f, "cg recorded");
    // save -> load -> render twice identical, and equal to direct render
    CHECK(d1.save_to_file(dir + "/s5.spdt"), "dataset saved");
    Spectral::SpectralDataset loaded;
    CHECK(loaded.load_from_file(dir + "/s5.spdt"), "dataset loaded");
    CHECK(loaded == d1, "reload semantic equality");
    CHECK(loaded.dataset_identity() == d1.dataset_identity(), "identity survives reload");
    CHECK(Spectral::render_dataset(cfg, loaded).ok(), "render from loaded");
    std::vector<uint8_t> png_loaded, png_direct, png_again;
    CHECK(read_bytes(dir + "/s5_out.png", png_loaded), "read loaded-render");
    cfg.output_path = dir + "/s5_direct.png";
    CHECK(Spectral::render_dataset(cfg, d1).ok(), "render from direct");
    CHECK(read_bytes(dir + "/s5_direct.png", png_direct), "read direct-render");
    CHECK(png_loaded == png_direct, "loaded vs direct bytes identical");
    cfg.output_path = dir + "/s5_out.png";
    CHECK(Spectral::render_dataset(cfg, loaded).ok(), "render again");
    CHECK(read_bytes(dir + "/s5_out.png", png_again), "read re-render");
    CHECK(png_again == png_loaded, "render deterministic");
    // bridge fingerprint stability across identical runs
    Spectral::DecodedMedia m;
    m.file_path = cfg.input_path;
    std::string hash_err;
    CHECK(Spectral::sha256_file(cfg.input_path, m.file_hash, hash_err), "hash file");
    std::error_code fec;
    m.file_size_bytes = fs::file_size(cfg.input_path, fec);
    CHECK(!fec, "file size readable");
    m.sample_rate = 44100;
    m.num_channels = 1;
    m.duration_seconds = 1.0;
    m.codec_name = "pcm_s16le";
    const Spectral::ProjectConfig p1 = Spectral::make_project_config(cfg, m);
    const Spectral::ProjectConfig p2 = Spectral::make_project_config(cfg, m);
    CHECK(p1.analysis_fingerprint() == p2.analysis_fingerprint(), "bridge deterministic");
    fs::remove_all(dir);
    std::printf("\n=== reproducibility: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
