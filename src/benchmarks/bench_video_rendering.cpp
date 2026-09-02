#include "benchmark_harness.h"
#include "../core/media/media_decoder.h"
#include "../core/rendering/spectrogram_renderer.h"
#include "../core/rendering/png_encoder.h"
#include "../core/dsp/fft.h"
#include <string>
#include <cstdio>
#include <vector>

bench::Result bench_video_rendering(int sample_rate, int fft_size, int duration_sec) {
    bench::Result r;
    r.benchmark = "video_rendering";
    r.sample_rate = sample_rate;
    r.fft_size = fft_size;
    r.duration_sec = duration_sec;

    std::string wav_path = "bench_video_" + std::to_string(sample_rate) + ".wav";
    int num_samples = sample_rate * duration_sec;
    bench::generate_wav(wav_path, sample_rate, num_samples);

    MediaDecoder decoder;
    decoder.open(wav_path);

    Spectral::SpectrogramConfig cfg;
    cfg.width = 1920;
    cfg.height = 1080;
    Spectral::SpectrogramRenderer renderer(cfg);
    Spectral::RGBAImage image;

    Spectral::SpectralDataset dataset;
    auto& am = dataset.mutable_analysis_metadata();
    am.sample_rate = sample_rate;
    am.fft_size = fft_size;
    am.hop_size = fft_size / 4;
    auto& fa = dataset.mutable_frequency_axis();
    fa = Spectral::FrequencyAxis(fft_size, sample_rate);

    int frame_count = 0;
    auto cpu0 = bench::cpu_time_ms();
    auto wall = bench::bench_fn([&]() {
        AudioFrame aframe;
        int hop = fft_size / 4;
        auto window = window_hann(fft_size);
        while (decoder.read_frame(aframe)) {
            int samples_per_frame = static_cast<int>(aframe.samples.size()) / aframe.num_channels;
            for (int pos = 0; pos + fft_size <= samples_per_frame; pos += hop) {
                std::vector<complex_f> spectrum(fft_size);
                for (int i = 0; i < fft_size; ++i) {
                    spectrum[i] = {aframe.samples[(pos + i) * aframe.num_channels] * window[i], 0.0f};
                }
                fft(spectrum);
                Spectral::SpectralFrame sf;
                sf.n_fft = fft_size;
                sf.magnitudes.resize(fft_size / 2 + 1);
                for (int k = 0; k < fft_size / 2 + 1; ++k) {
                    float re = spectrum[k].real();
                    float im = spectrum[k].imag();
                    sf.magnitudes[k] = std::sqrt(re * re + im * im);
                }
                dataset.add_frame(sf);
                frame_count++;
            }
            renderer.render(dataset, image);
        }
    }, 2);
    auto cpu1 = bench::cpu_time_ms();

    remove(wav_path.c_str());

    r.wall_ms = wall;
    r.throughput_mbs = frame_count > 0 ? (static_cast<double>(frame_count) * image.pixels.size()) / (wall / 1000.0) / (1024.0 * 1024.0) : 0.0;
    r.peak_memory_mb = bench::peak_memory_mb();
    r.cpu_time_ms = cpu1 - cpu0;
    r.extra = "frames=" + std::to_string(frame_count);
    return r;
}
