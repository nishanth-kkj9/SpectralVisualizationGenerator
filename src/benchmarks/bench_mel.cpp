#include "benchmark_harness.h"
#include "../core/pipeline/pipeline.h"

#include <cstdio>
#include <string>

// Phase 9 — Mel representation benchmark.
//
// Measures the REAL streaming pipeline end to end (decode -> bounded mono
// overlap buffer -> STFT frame -> Mel filterbank -> SpectralDataset), the
// same way bench_streaming_analysis measures STFT: every number is measured
// in this process, nothing is estimated.
//
// Reported:
//   * headline wall_ms/throughput = 64-band Slaney Mel through the harness
//     (one warmup + one measured pass; each pass spawns a fresh ffmpeg child).
//   * extra = measured frame counts (Mel must equal STFT framing), the peak
//     live raw-audio buffer, and single-pass wall times for 32 bands, 128
//     bands, and the STFT reference at the identical rate/FFT size — the
//     comparison the spec asks for is only meaningful when rate, FFT size,
//     hop, window and duration are identical, which they are here.
namespace {

struct PassResult {
    bool ok = false;
    double wall_ms = 0.0;
    int frames = 0;
    size_t peak_buf_samples = 0;
};

// One complete analysis pass: decode + represent, fresh ffmpeg child each
// time (so the decode cost is included and shared by every variant).
PassResult run_pass(const Spectral::GenerateConfig& cfg) {
    PassResult out;
    Spectral::SpectralDataset ds;
    Spectral::AnalyzeStats stats;
    const auto t0 = std::chrono::high_resolution_clock::now();
    const Spectral::Error err =
        Spectral::analyze_dataset(cfg, ds, {}, nullptr, 0, &stats);
    const auto t1 = std::chrono::high_resolution_clock::now();
    out.ok = err.ok();
    out.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    out.frames = ds.frame_count();
    out.peak_buf_samples = stats.peak_buffered_samples;
    return out;
}

std::string ms_str(double ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", ms);
    return buf;
}

} // namespace

bench::Result bench_mel(int sample_rate, int fft_size, int duration_sec) {
    bench::Result r;
    r.benchmark = "mel";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;

    const std::string wav_path = "bench_mel_" + std::to_string(sample_rate) + "_" +
                                 std::to_string(duration_sec) + ".wav";
    const int num_samples = sample_rate * duration_sec;
    bench::generate_wav(wav_path, sample_rate, num_samples);

    auto make_cfg = [&](const std::string& rep, int bands, const std::string& norm) {
        Spectral::GenerateConfig cfg;
        cfg.input_path = wav_path;
        cfg.output_path = "bench_mel_out.png";  // validated, never written here
        cfg.fft_size = fft_size;
        cfg.width = 256;
        cfg.height = 128;
        cfg.representation = rep;
        cfg.mel_bands = bands;
        cfg.mel_norm = norm;
        return cfg;
    };

    // Headline: default 64-band Slaney Mel, harness median (warmup + measured).
    PassResult head;
    r.iterations = 1;
    const auto cpu0 = bench::cpu_time_ms();
    const double wall = bench::bench_fn([&]() { head = run_pass(make_cfg("mel", 64, "slaney")); }, 1);
    const auto cpu1 = bench::cpu_time_ms();

    // Variants and the STFT reference: single measured passes (documented
    // as such in `extra`, never presented as harness medians).
    const PassResult mel32 = run_pass(make_cfg("mel", 32, "slaney"));
    const PassResult mel128 = run_pass(make_cfg("mel", 128, "slaney"));
    const PassResult stft = run_pass(make_cfg("stft", 64, "slaney"));

    std::remove(wav_path.c_str());

    r.wall_ms = wall;
    r.throughput_mbs = (static_cast<double>(num_samples) * sizeof(float)) /
                       (wall / 1000.0) / (1024.0 * 1024.0);
    r.peak_memory_mb = bench::peak_memory_mb();
    r.cpu_time_ms = cpu1 - cpu0;
    r.extra = "ok=" + std::string(head.ok ? "1" : "0") +
              " bands=64 frames=" + std::to_string(head.frames) +
              " peak_buf_kb=" +
              std::to_string(static_cast<long long>(head.peak_buf_samples) *
                             static_cast<long long>(sizeof(float)) / 1024) +
              " mel32_wall_ms=" + ms_str(mel32.wall_ms) +
              " mel128_wall_ms=" + ms_str(mel128.wall_ms) +
              " stft_wall_ms=" + ms_str(stft.wall_ms) +
              " stft_frames=" + std::to_string(stft.frames);
    return r;
}
