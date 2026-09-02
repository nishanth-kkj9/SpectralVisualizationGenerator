#include "benchmark_harness.h"
#include "../core/dsp/fft.h"
#include <vector>
#include <string>

bench::Result bench_fft(int sample_rate, int fft_size, int duration_sec) {
    bench::Result r;
    r.benchmark = "fft";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;

    int num_samples = sample_rate * duration_sec;
    auto audio = bench::generate_audio(sample_rate, num_samples);
    std::vector<complex_f> spectrum(fft_size);
    auto window = window_hann(fft_size);

    int num_blocks = (num_samples - fft_size) / fft_size + 1;
    r.iterations = num_blocks;

    auto cpu0 = bench::cpu_time_ms();
    auto wall = bench::bench_fn([&]() {
        for (int b = 0; b < num_blocks; ++b) {
            int offset = b * fft_size;
            for (int i = 0; i < fft_size; ++i) {
                spectrum[i] = {audio[offset + i] * window[i], 0.0f};
            }
            fft(spectrum);
        }
    });
    auto cpu1 = bench::cpu_time_ms();

    r.wall_ms = wall;
    r.throughput_mbs = (static_cast<double>(num_samples) * sizeof(float)) / (wall / 1000.0) / (1024.0 * 1024.0);
    r.peak_memory_mb = bench::peak_memory_mb();
    r.cpu_time_ms = cpu1 - cpu0;
    return r;
}
