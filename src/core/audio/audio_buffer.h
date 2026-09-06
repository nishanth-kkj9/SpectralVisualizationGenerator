#pragma once

#include <vector>
#include <cstddef>
#include <algorithm>

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

    // Mix down to mono (average channels)
    void mix_down() {
        if (num_channels <= 1) return;
        float sum = 0.0f;
        for (int ch = 0; ch < num_channels; ++ch) {
            for (int i = 0; i < size(); ++i) {
                sum += at(ch, i);
            }
        }
        const float avg = sum / (num_channels * size());
        // Replace with mono
        data.assign(size(), avg);
        num_channels = 1;
    }

    // Resample (placeholder - linear interpolation stub)
    void resample(int target_sr) {
        (void)target_sr;
        // TODO: proper resampling
    }
};