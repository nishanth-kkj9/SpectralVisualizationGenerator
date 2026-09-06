#pragma once

#include <vector>
#include <cstdint>

struct SpectralFrame {
    int frame_index;
    int n_fft;
    float window_factor;
    std::vector<float> magnitudes;  // magnitude spectrum
    std::vector<float> phases;      // phase spectrum
};

class SpectralAnalyzer {
public:
    virtual ~SpectralAnalyzer() = default;

    // Configure analyzer
    virtual void set_fft_size(int n_fft) = 0;
    virtual void set_window(const std::vector<float>& window) = 0;
    virtual void set_overlap(float ratio) = 0;

    // Analyze one frame of audio
    virtual bool analyze_frame(const float* samples, int num_samples, SpectralFrame& out) = 0;

    // Analyze from AudioBuffer
    virtual bool analyze_buffer(const class AudioBuffer& buf, SpectralFrame& out) = 0;

    // Get the number of frequency bins (excluding DC and Nyquist)
    virtual int freq_bins() const = 0;

    // Get the maximum frequency represented
    virtual float max_frequency() const = 0;
};