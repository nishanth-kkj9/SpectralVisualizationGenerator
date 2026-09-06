#pragma once

#include "spectral_analyzer.h"
#include "dsp/fft.h"
#include "dsp/windows.h"

#include <vector>
#include <cmath>
#include <algorithm>

class ConcreteSpectralAnalyzer : public SpectralAnalyzer {
public:
    ConcreteSpectralAnalyzer();

    // Configure analyzer
    void set_fft_size(int n_fft) override;
    void set_window(const std::vector<float>& window) override;
    void set_overlap(float ratio) override;

    // Analyze one frame of audio
    bool analyze_frame(const float* samples, int num_samples, SpectralFrame& out) override;

    // Analyze from AudioBuffer
    bool analyze_buffer(const AudioBuffer& buf, SpectralFrame& out) override;

    // Get the number of frequency bins (excluding DC and Nyquist)
    int freq_bins() const override;

    // Get the maximum frequency represented
    float max_frequency() const override;

private:
    int fft_size_{1024};
    int hop_size_{512};
    float overlap_{0.5f};
    std::vector<float> window_;
    int sample_rate_{44100};

    // Apply window and FFT to a segment
    bool analyze_segment(const float* samples, int start_idx, SpectralFrame& out);

    // Apply window to samples
    std::vector<complex_f> apply_window(const float* samples, int start_idx, int n_fft);

    // Compute magnitude/power from complex FFT result
    void compute_spectrum(const std::vector<complex_f>& X, SpectralFrame& out);
};