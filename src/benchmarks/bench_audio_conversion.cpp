#include "benchmark_harness.h"
#include "../core/audio/audio_buffer.h"
#include <vector>
#include <string>

bench::Result bench_audio_conversion(int sample_rate, int fft_size, int duration_sec) {
    bench::Result r;
    r.benchmark = "audio_conversion";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;

    int num_samples = sample_rate * duration_sec;
    int num_channels = 2;
    std::vector<float> interleaved(num_samples * num_channels);
    for (int i = 0; i < num_samples; ++i) {
        float t = static_cast<float>(i) / sample_rate;
        float s = std::sin(2.0f * 3.14159265f * 440.0f * t);
        interleaved[i * 2] = s;
        interleaved[i * 2 + 1] = s * 0.8f;
    }

    auto cpu0 = bench::cpu_time_ms();
    auto wall = bench::bench_fn([&]() {
        AudioBuffer buf(sample_rate, num_channels);
        buf.data = interleaved;
        buf.mix_down();
    }, 3);
    auto cpu1 = bench::cpu_time_ms();

    r.wall_ms = wall;
    r.throughput_mbs = (static_cast<double>(num_samples * num_channels * sizeof(float))) / (wall / 1000.0) / (1024.0 * 1024.0);
    r.peak_memory_mb = bench::peak_memory_mb();
    r.cpu_time_ms = cpu1 - cpu0;
    return r;
}
