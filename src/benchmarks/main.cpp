#include "benchmark_harness.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <algorithm>

// Forward declarations from bench_*.cpp files
bench::Result bench_fft(int sample_rate, int fft_size, int duration_sec);
bench::Result bench_stft(int sample_rate, int fft_size, int duration_sec);
bench::Result bench_dataset_serialization(int sample_rate, int fft_size, int duration_sec);
bench::Result bench_image_rendering(int sample_rate, int fft_size, int duration_sec);
bench::Result bench_image_rendering_1920(int sample_rate, int fft_size, int duration_sec);
bench::Result bench_png_encoding(int sample_rate, int fft_size, int duration_sec);
bench::Result bench_audio_conversion(int sample_rate, int fft_size, int duration_sec);
bench::Result bench_media_decode(int sample_rate, int fft_size, int duration_sec);
bench::Result bench_video_rendering(int sample_rate, int fft_size, int duration_sec);
bench::Result bench_backend(int sample_rate, int fft_size, int duration_sec);

static void print_usage() {
    printf("Usage: spectral_benchmarks [OPTIONS]\n\n");
    printf("Options:\n");
    printf("  --quick             Run quick benchmark (1-minute durations only)\n");
    printf("  --benchmark NAME    Run only the named benchmark (e.g. fft, stft)\n");
    printf("  --output PATH       Write JSON results to PATH\n");
    printf("  --help              Show this help\n");
}

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

int main(int argc, char* argv[]) {
    bool quick = false;
    std::string filter;
    std::string output_path = "benchmarks/results.json";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else if (std::strcmp(argv[i], "--benchmark") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage();
            return 1;
        }
    }

    // Parameter matrix
    int sample_rates[] = {44100, 48000, 96000};
    int fft_sizes[] = {2048, 4096, 8192, 16384, 32768};
    int durations[] = {60, 600, 3600};
    int quick_durations[] = {60};

    int num_durations = quick ? 1 : 3;
    int* dur_ptr = quick ? quick_durations : durations;

    printf("=== SpectralVisualizationGenerator Performance Baseline ===\n");
    printf("Mode: %s\n", quick ? "quick (1-minute durations)" : "full");
    printf("Sample rates: 44100, 48000, 96000\n");
    printf("FFT sizes: 2048, 4096, 8192, 16384, 32768\n");
    printf("Durations: %s\n", quick ? "60s" : "60s, 600s, 3600s");
    printf("========================================================\n\n");

    std::vector<bench::Result> results;

    auto run_if = [&](const std::string& name, auto fn) {
        if (!filter.empty() && filter != name) return;
        printf("Running %s...\n", name.c_str());
        auto r = fn();
        results.push_back(r);
        printf("  wall=%.1fms throughput=%.2f MB/s peak_mem=%.1fMB cpu=%.1fms\n",
               r.wall_ms, r.throughput_mbs, r.peak_memory_mb, r.cpu_time_ms);
    };

    for (int sr : sample_rates) {
        for (int fs : fft_sizes) {
            for (int d_idx = 0; d_idx < num_durations; ++d_idx) {
                int dur = dur_ptr[d_idx];
                run_if("fft", [&]() { return bench_fft(sr, fs, dur); });
                run_if("stft", [&]() { return bench_stft(sr, fs, dur); });
                run_if("dataset_serialization", [&]() { return bench_dataset_serialization(sr, fs, dur); });
            }
        }
        // Resolution-dependent benchmarks (fixed fft_size=4096, duration=60s)
        run_if("image_rendering", [&]() { return bench_image_rendering(sr, 4096, 60); });
        run_if("image_rendering_1920", [&]() { return bench_image_rendering_1920(sr, 4096, 60); });
        run_if("png_encoding", [&]() { return bench_png_encoding(sr, 4096, 60); });
        run_if("audio_conversion", [&]() { return bench_audio_conversion(sr, 4096, 60); });
        run_if("media_decode", [&]() { return bench_media_decode(sr, 4096, 60); });
        run_if("video_rendering", [&]() { return bench_video_rendering(sr, 4096, 60); });
        run_if("backend", [&]() { return bench_backend(sr, 4096, 60); });
    }

    // Write JSON output
    printf("\nWriting results to %s...\n", output_path.c_str());
    FILE* f = nullptr;
    fopen_s(&f, output_path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "Failed to open output file: %s\n", output_path.c_str());
        return 1;
    }

    fprintf(f, "{\n  \"results\": [\n");
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"benchmark\": \"%s\",\n", json_escape(r.benchmark).c_str());
        fprintf(f, "      \"sample_rate\": %d,\n", r.sample_rate);
        fprintf(f, "      \"fft_size\": %d,\n", r.fft_size);
        fprintf(f, "      \"duration_sec\": %d,\n", r.duration_sec);
        if (!r.extra.empty()) fprintf(f, "      \"extra\": \"%s\",\n", json_escape(r.extra).c_str());
        fprintf(f, "      \"wall_ms\": %.3f,\n", r.wall_ms);
        fprintf(f, "      \"throughput_mbs\": %.3f,\n", r.throughput_mbs);
        fprintf(f, "      \"peak_memory_mb\": %.3f,\n", r.peak_memory_mb);
        fprintf(f, "      \"cpu_time_ms\": %.3f,\n", r.cpu_time_ms);
        fprintf(f, "      \"iterations\": %d\n", r.iterations);
        fprintf(f, "    }%s\n", i + 1 < results.size() ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);

    printf("Done. %zu benchmark results written.\n", results.size());
    return 0;
}
