#include "benchmark_harness.h"
#include "cpu_backend.h"
#include "fft.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

bench::Result bench_backend(int sample_rate, int fft_size, int duration_sec) {
    using namespace bench;

    Spectral::CPUBackend backend;
    int num_samples = sample_rate * duration_sec;
    int n_fft = fft_size;

    // Generate test signal
    std::vector<float> input(num_samples);
    for (int i = 0; i < num_samples; ++i) {
        input[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / static_cast<float>(sample_rate));
    }

    const int outer = 3;
    const int inner = 5;
    std::vector<double> wall_times(outer);
    std::vector<double> cpu_times(outer);

    double mem_before = bench::peak_memory_mb();

    for (int o = 0; o < outer; ++o) {
        auto w0 = std::chrono::high_resolution_clock::now();
        auto c0 = std::chrono::steady_clock::now();

        for (int k = 0; k < inner; ++k) {
            int batch_size = (num_samples / n_fft);
            if (batch_size < 1) batch_size = 1;

            std::vector<complex_f> output(batch_size * (n_fft / 2 + 1));
            backend.fft_batch(input.data(), output.data(), n_fft, batch_size);

            // Benchmark single-frame fft + magnitude_power
            std::vector<complex_f> frame(n_fft);
            for (int i = 0; i < n_fft; ++i) {
                frame[i] = complex_f(input[i], 0.0f);
            }
            backend.fft(frame);
            backend.fft_magnitude_power(frame);
        }

        auto c1 = std::chrono::steady_clock::now();
        auto w1 = std::chrono::high_resolution_clock::now();

        wall_times[o] = std::chrono::duration<double, std::milli>(w1 - w0).count();
        cpu_times[o] = std::chrono::duration<double, std::milli>(c1 - c0).count();
    }

    double mem_after = bench::peak_memory_mb();

    std::sort(wall_times.begin(), wall_times.end());
    std::sort(cpu_times.begin(), cpu_times.end());

    double wall_ms = wall_times[1];
    double cpu_ms = cpu_times[1];

    int total_frames = (num_samples / n_fft);
    size_t bytes = static_cast<size_t>(num_samples) * sizeof(float);
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    double throughput = (wall_ms > 0) ? mb / (wall_ms / 1000.0) : 0;

    Result r;
    r.benchmark = "backend";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;
    r.wall_ms = wall_ms;
    r.throughput_mbs = throughput;
    r.peak_memory_mb = std::max(mem_before, mem_after);
    r.cpu_time_ms = cpu_ms;
    r.iterations = inner;

    return r;
}
