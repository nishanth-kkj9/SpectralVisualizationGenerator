#define NOMINMAX
#pragma once
#include <chrono>
#include <vector>
#include <string>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace bench {

struct Result {
    std::string benchmark;
    int sample_rate = 0;
    int fft_size = 0;
    int duration_sec = 0;
    std::string extra;
    double wall_ms = 0.0;
    double throughput_mbs = 0.0;
    double peak_memory_mb = 0.0;
    double cpu_time_ms = 0.0;
    int iterations = 0;
};

inline double peak_memory_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
#endif
    return 0.0;
}

inline double cpu_time_ms() {
#ifdef _WIN32
    FILETIME creation, exit_ft, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_ft, &kernel, &user)) {
        auto to_ms = [](const FILETIME& ft) -> double {
            ULARGE_INTEGER li;
            li.LowPart = ft.dwLowDateTime;
            li.HighPart = ft.dwHighDateTime;
            return static_cast<double>(li.QuadPart) / 10000.0;
        };
        return to_ms(kernel) + to_ms(user);
    }
#endif
    return 0.0;
}

inline double bench_fn(std::function<void()> fn, int iterations = 3) {
    fn(); // warmup
    std::vector<double> times;
    for (int i = 0; i < iterations; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();
        fn();
        auto t1 = std::chrono::high_resolution_clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

inline std::vector<float> generate_audio(int sample_rate, int num_samples) {
    std::vector<float> samples(num_samples);
    const float freqs[] = {440.0f, 880.0f, 1760.0f, 3520.0f};
    const float amps[] = {0.25f, 0.25f, 0.25f, 0.25f};
    for (int i = 0; i < num_samples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(sample_rate);
        float s = 0.0f;
        for (int f = 0; f < 4; ++f) {
            s += amps[f] * std::sin(2.0f * 3.14159265f * freqs[f] * t);
        }
        samples[i] = s;
    }
    return samples;
}

inline std::string generate_wav(const std::string& path, int sample_rate, int num_samples) {
    auto samples = generate_audio(sample_rate, num_samples);
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "wb");
    if (!f) return "";

    int num_channels = 1;
    int bits_per_sample = 16;
    int byte_rate = sample_rate * num_channels * bits_per_sample / 8;
    int block_align = num_channels * bits_per_sample / 8;
    int data_size = num_samples * block_align;

    fwrite("RIFF", 1, 4, f);
    uint32_t chunk_size = 36 + data_size;
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_format = 1;
    fwrite(&audio_format, 2, 1, f);
    uint16_t ch = static_cast<uint16_t>(num_channels);
    fwrite(&ch, 2, 1, f);
    uint32_t sr = static_cast<uint32_t>(sample_rate);
    fwrite(&sr, 4, 1, f);
    uint32_t br = static_cast<uint32_t>(byte_rate);
    fwrite(&br, 4, 1, f);
    uint16_t ba = static_cast<uint16_t>(block_align);
    fwrite(&ba, 2, 1, f);
    uint16_t bps = static_cast<uint16_t>(bits_per_sample);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    uint32_t ds = static_cast<uint32_t>(data_size);
    fwrite(&ds, 4, 1, f);

    for (int i = 0; i < num_samples; ++i) {
        float clamped = std::max(-1.0f, std::min(1.0f, samples[i]));
        int16_t s = static_cast<int16_t>(clamped * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return path;
}

} // namespace bench
