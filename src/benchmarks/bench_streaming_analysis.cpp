#include "benchmark_harness.h"
#include "../core/pipeline/pipeline.h"

#include <cstdio>
#include <string>

// Phase 6 — end-to-end streaming analysis: decode + overlap-buffered STFT
// into a SpectralDataset. Reports audio throughput, process peak RSS, and
// (in extra) the peak live raw-audio buffer. Single iteration: each run
// re-decodes through a real ffmpeg child, so repeats cost minutes.
bench::Result bench_streaming_analysis(int sample_rate, int fft_size, int duration_sec) {
    bench::Result r;
    r.benchmark = "streaming_analysis";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;

    const std::string wav_path =
        "bench_stream_" + std::to_string(sample_rate) + "_" + std::to_string(duration_sec) +
        ".wav";
    const int num_samples = sample_rate * duration_sec;
    bench::generate_wav(wav_path, sample_rate, num_samples);

    Spectral::GenerateConfig cfg;
    cfg.input_path = wav_path;
    cfg.output_path = "bench_stream_out.png";  // validated, never written here
    cfg.fft_size = fft_size;
    cfg.width = 256;
    cfg.height = 128;

    Spectral::AnalyzeStats stats;
    int frames = 0;
    r.iterations = 1;
    auto cpu0 = bench::cpu_time_ms();
    auto wall = bench::bench_fn(
        [&]() {
            Spectral::SpectralDataset ds;
            Spectral::AnalyzeStats s;
            if (Spectral::analyze_dataset(cfg, ds, {}, nullptr, 0, &s).ok()) {
                frames = ds.frame_count();
                stats = s;
            }
        },
        1);
    auto cpu1 = bench::cpu_time_ms();
    remove(wav_path.c_str());

    r.wall_ms = wall;
    r.throughput_mbs = (static_cast<double>(num_samples) * sizeof(float)) / (wall / 1000.0) /
                       (1024.0 * 1024.0);
    r.peak_memory_mb = bench::peak_memory_mb();
    r.cpu_time_ms = cpu1 - cpu0;
    r.extra = "frames=" + std::to_string(frames) + " peak_buf_kb=" +
              std::to_string(stats.peak_buffered_samples * sizeof(float) / 1024);
    return r;
}
