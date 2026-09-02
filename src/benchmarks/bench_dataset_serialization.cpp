#include "benchmark_harness.h"
#include "../core/spectral/spectral_dataset.h"
#include "../core/dsp/fft.h"
#include <vector>
#include <string>

static Spectral::SpectralDataset build_dataset(int sample_rate, int fft_size, int duration_sec) {
    Spectral::SpectralDataset dataset;
    auto& am = dataset.mutable_analysis_metadata();
    am.sample_rate = sample_rate;
    am.fft_size = fft_size;
    am.hop_size = fft_size / 4;
    am.num_frequency_bins = fft_size / 2 + 1;
    am.nyquist_frequency = static_cast<float>(sample_rate) / 2.0f;
    am.total_duration_seconds = static_cast<double>(duration_sec);
    am.total_frames = 0;

    auto& fa = dataset.mutable_frequency_axis();
    fa = Spectral::FrequencyAxis(fft_size, sample_rate);
    auto& ta = dataset.mutable_time_axis();
    int num_samples = sample_rate * duration_sec;
    int hop = fft_size / 4;
    int num_frames = (num_samples - fft_size) / hop + 1;
    ta = Spectral::TimeAxis(num_frames, hop, sample_rate);

    auto audio = bench::generate_audio(sample_rate, num_samples);
    auto window = window_hann(fft_size);
    for (int f = 0; f < num_frames; ++f) {
        int offset = f * hop;
        std::vector<complex_f> spectrum(fft_size);
        for (int i = 0; i < fft_size; ++i) {
            spectrum[i] = {audio[offset + i] * window[i], 0.0f};
        }
        fft(spectrum);
        Spectral::SpectralFrame frame;
        frame.frame_index = f;
        frame.n_fft = fft_size;
        frame.timestamp = static_cast<double>(offset) / sample_rate;
        frame.magnitudes.resize(fft_size / 2 + 1);
        frame.phases.resize(fft_size / 2 + 1);
        frame.power.resize(fft_size / 2 + 1);
        float rms = 0.0f;
        float peak = 0.0f;
        for (int k = 0; k < fft_size / 2 + 1; ++k) {
            float re = spectrum[k].real();
            float im = spectrum[k].imag();
            float mag = std::sqrt(re * re + im * im);
            frame.magnitudes[k] = mag;
            frame.phases[k] = std::atan2(im, re);
            frame.power[k] = mag * mag;
            rms += mag * mag;
            if (mag > peak) peak = mag;
        }
        frame.rms = std::sqrt(rms / (fft_size / 2 + 1));
        frame.peak_magnitude = peak;
        dataset.add_frame(frame);
    }
    return dataset;
}

bench::Result bench_dataset_serialization(int sample_rate, int fft_size, int duration_sec) {
    bench::Result r;
    r.benchmark = "dataset_serialization";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;

    auto dataset = build_dataset(sample_rate, fft_size, duration_sec);
    std::vector<uint8_t> buffer;
    r.iterations = 1;

    auto cpu0 = bench::cpu_time_ms();
    auto wall = bench::bench_fn([&]() {
        dataset.serialize_binary(buffer);
    }, 3);
    auto cpu1 = bench::cpu_time_ms();

    r.wall_ms = wall;
    r.throughput_mbs = static_cast<double>(buffer.size()) / (wall / 1000.0) / (1024.0 * 1024.0);
    r.peak_memory_mb = bench::peak_memory_mb();
    r.cpu_time_ms = cpu1 - cpu0;
    return r;
}
