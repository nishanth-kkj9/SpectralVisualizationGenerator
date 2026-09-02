#include "benchmark_harness.h"
#include "../core/media/media_decoder.h"
#include <string>
#include <cstdio>

bench::Result bench_media_decode(int sample_rate, int fft_size, int duration_sec) {
    bench::Result r;
    r.benchmark = "media_decode";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;

    std::string wav_path = "bench_test_" + std::to_string(sample_rate) + ".wav";
    int num_samples = sample_rate * duration_sec;
    bench::generate_wav(wav_path, sample_rate, num_samples);

    r.iterations = 1;
    auto cpu0 = bench::cpu_time_ms();
    auto wall = bench::bench_fn([&]() {
        MediaDecoder decoder;
        decoder.open(wav_path);
        AudioFrame frame;
        while (decoder.read_frame(frame)) {}
    }, 3);
    auto cpu1 = bench::cpu_time_ms();

    remove(wav_path.c_str());

    r.wall_ms = wall;
    r.throughput_mbs = (static_cast<double>(num_samples * sizeof(float))) / (wall / 1000.0) / (1024.0 * 1024.0);
    r.peak_memory_mb = bench::peak_memory_mb();
    r.cpu_time_ms = cpu1 - cpu0;
    return r;
}
