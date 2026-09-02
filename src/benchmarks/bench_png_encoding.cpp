#include "benchmark_harness.h"
#include "../core/rendering/png_encoder.h"
#include <vector>
#include <string>

bench::Result bench_png_encoding(int sample_rate, int fft_size, int duration_sec) {
    (void)sample_rate; (void)fft_size; (void)duration_sec;
    bench::Result r;
    r.benchmark = "png_encoding";
    r.iterations = 1;

    int w = 1920, h = 1080;
    std::vector<uint8_t> pixels(w * h * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = static_cast<uint8_t>((i / 4) % 256);
        pixels[i+1] = static_cast<uint8_t>(((i / 4) * 7) % 256);
        pixels[i+2] = static_cast<uint8_t>(((i / 4) * 13) % 256);
        pixels[i+3] = 255;
    }
    std::vector<uint8_t> png;

    auto cpu0 = bench::cpu_time_ms();
    auto wall = bench::bench_fn([&]() {
        Spectral::PNGEncoder::encode_rgba(png, w, h, pixels.data());
    }, 3);
    auto cpu1 = bench::cpu_time_ms();

    r.wall_ms = wall;
    r.throughput_mbs = static_cast<double>(png.size()) / (wall / 1000.0) / (1024.0 * 1024.0);
    r.peak_memory_mb = bench::peak_memory_mb();
    r.cpu_time_ms = cpu1 - cpu0;
    r.extra = "1920x1080";
    return r;
}
