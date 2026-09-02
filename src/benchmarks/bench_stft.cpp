#include "benchmark_harness.h"
#include "../core/dsp/fft.h"
#include <vector>
#include <string>

bench::Result bench_stft(int sample_rate, int fft_size, int duration_sec) {
    bench::Result r;
    r.benchmark = "stft";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;

    int num_samples = sample_rate * duration_sec;
    auto audio = bench::generate_audio(sample_rate, num_samples);
    int hop = fft_size / 4;
    auto window = window_hann(fft_size);
    int num_frames = (num_samples - fft_size) / hop + 1;
    r.iterations = num_frames;

    std::vector<std::vector<float>> spectrogram(num_frames, std::vector<float>(fft_size / 2 + 1));

    auto cpu0 = bench::cpu_time_ms();
    auto wall = bench::bench_fn([&]() {
        for (int f = 0; f < num_frames; ++f) {
            int offset = f * hop;
            std::vector<complex_f> spectrum(fft_size);
            for (int i = 0; i < fft_size; ++i) {
                spectrum[i] = {audio[offset + i] * window[i], 0.0f};
            }
            fft(spectrum);
            for (int k = 0; k < fft_size / 2 + 1; ++k) {
                spectrogram[f][k] = std::sqrt(spectrum[k].real() * spectrum[k].real() + spectrum[k].imag() * spectrum[k].imag());
            }
        }
    });
    auto cpu1 = bench::cpu_time_ms();

    r.wall_ms = wall;
    r.throughput_mbs = (static_cast<double>(num_samples) * sizeof(float)) / (wall / 1000.0) / (1024.0 * 1024.0);
    r.peak_memory_mb = bench::peak_memory_mb();
    r.cpu_time_ms = cpu1 - cpu0;
    return r;
}
