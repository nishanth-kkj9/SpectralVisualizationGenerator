#include "benchmark_harness.h"
#include "../core/rendering/spectrogram_renderer.h"
#include "../core/spectral/spectral_dataset.h"
#include "../core/dsp/fft.h"
#include <cstdio>
#include <string>
#include <vector>

static Spectral::SpectralDataset build_dataset(int sample_rate, int fft_size, int n_frames) {
    Spectral::SpectralDataset dataset;
    auto& am = dataset.mutable_analysis_metadata();
    am.sample_rate = sample_rate;
    am.fft_size = fft_size;
    am.hop_size = fft_size / 4;
    am.num_frequency_bins = fft_size / 2 + 1;
    am.nyquist_frequency = static_cast<float>(sample_rate) / 2.0f;
    dataset.mutable_normalization_info().reference_amplitude = 1.0f;
    dataset.mutable_frequency_axis() = Spectral::FrequencyAxis(fft_size, sample_rate);
    dataset.mutable_time_axis() = Spectral::TimeAxis(n_frames, fft_size / 4, sample_rate);
    auto audio = bench::generate_audio(sample_rate, n_frames * (fft_size / 4) + fft_size);
    auto window = window_hann(fft_size);
    for (int f = 0; f < n_frames; ++f) {
        int offset = f * (fft_size / 4);
        std::vector<complex_f> spectrum(fft_size);
        for (int i = 0; i < fft_size; ++i) spectrum[i] = {audio[offset + i] * window[i], 0.0f};
        fft(spectrum);
        Spectral::SpectralFrame frame;
        frame.frame_index = f;
        frame.n_fft = fft_size;
        frame.timestamp = static_cast<double>(offset) / sample_rate;
        frame.magnitudes.resize(fft_size / 2 + 1);
        for (int k = 0; k < fft_size / 2 + 1; ++k) {
            float re = spectrum[k].real(), im = spectrum[k].imag();
            frame.magnitudes[k] = std::sqrt(re * re + im * im);
        }
        dataset.add_frame(frame);
    }
    return dataset;
}

bench::Result bench_gpu_render(int sample_rate, int fft_size, int /*duration_sec*/) {
    bench::Result r;
    r.benchmark = "gpu_render";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;

    // ponytail: fixed 64-frame dataset; sweep resolution + colormap, not audio length
    auto dataset = build_dataset(sample_rate, fft_size, 64);
    const int widths[] = {512, 1024};
    const int heights[] = {256, 512};
    std::string summary;
    double cpu_total = 0.0, gpu_total = 0.0;

    for (int ri = 0; ri < 2; ++ri) {
        for (int cm = 0; cm < 2; ++cm) {
            Spectral::SpectrogramConfig cfg;
            cfg.width = widths[ri];
            cfg.height = heights[ri];
            cfg.color_map = static_cast<Spectral::ColorMap>(cm);
            Spectral::SpectrogramRenderer renderer(cfg);
            Spectral::RGBAImage img;
            double cpu_ms = bench::bench_fn([&]() { renderer.render(dataset, img); }, 2);
            double gpu_ms = bench::bench_fn([&]() { renderer.render_gpu(dataset, img); }, 2);
            cpu_total += cpu_ms;
            gpu_total += gpu_ms;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%dx%d/%s cpu=%.1f gpu=%.1f; ",
                          widths[ri], heights[ri], cm ? "heat" : "viridis", cpu_ms, gpu_ms);
            summary += buf;
        }
    }

    r.wall_ms = gpu_total;
    r.cpu_time_ms = cpu_total;
    r.throughput_mbs = 0.0;
    r.peak_memory_mb = bench::peak_memory_mb();
    r.iterations = 8;
    r.extra = summary;
    return r;
}
