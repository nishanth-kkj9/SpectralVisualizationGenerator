#pragma once

#include <vector>
#include <cstddef>
#include <algorithm>
#include <cmath>
#include <utility>

struct AudioBuffer {
    int sample_rate{44100};
    int num_channels{2};
    std::vector<float> data;  // interleaved: [s0_ch0, s0_ch1, s1_ch0, s1_ch1, ...]

    AudioBuffer() = default;

    AudioBuffer(int sr, int nc) : sample_rate(sr), num_channels(nc), data(sr * nc) {}

    // Number of samples per channel
    int size() const { return static_cast<int>(data.size() / num_channels); }

    // Access sample at (channel, index)
    float& at(int channel, int index) {
        return data[channel + index * num_channels];
    }

    const float& at(int channel, int index) const {
        return data[channel + index * num_channels];
    }

    // Get sample at planar index
    float& operator()(int channel, int index) { return at(channel, index); }

    // Mix down to mono: per-frame mean across channels.
    // output[t] = sum_ch(data[ch, t]) / num_channels.
    void mix_down() {
        if (num_channels <= 1) return;
        const int n = size();
        std::vector<float> mono(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            float sum = 0.0f;
            for (int ch = 0; ch < num_channels; ++ch) sum += at(ch, i);
            mono[static_cast<size_t>(i)] = sum / static_cast<float>(num_channels);
        }
        data = std::move(mono);
        num_channels = 1;
    }

    // Linear-interpolation resampler. Real conversion with documented
    // limits: exact output size round(n * target/source), DC preserved,
    // frequency response rolls off toward Nyquist (use decoder -ar for
    // production-quality band-limited conversion).
    void resample(int target_sr) {
        if (target_sr <= 0 || target_sr == sample_rate || data.empty()) return;
        const int n = size();
        const int out_n =
            static_cast<int>(std::llround(static_cast<long long>(n) * target_sr /
                                          static_cast<long long>(sample_rate)));
        if (out_n <= 0) return;
        std::vector<float> out(static_cast<size_t>(out_n) * static_cast<size_t>(num_channels));
        for (int i = 0; i < out_n; ++i) {
            const double pos = static_cast<double>(i) * (n - 1) /
                               static_cast<double>(out_n > 1 ? out_n - 1 : 1);
            const int lo = static_cast<int>(pos);
            const int hi = (lo + 1 < n) ? lo + 1 : lo;
            const float frac = static_cast<float>(pos - lo);
            for (int ch = 0; ch < num_channels; ++ch)
                out[static_cast<size_t>(ch + i * num_channels)] =
                    at(ch, lo) * (1.0f - frac) + at(ch, hi) * frac;
        }
        data = std::move(out);
        sample_rate = target_sr;
    }
};