#include "concrete_analyzer.h"
#include "dsp/fft.h"
#include "dsp/windows.h"

#include "audio_buffer.h"

#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <iostream>

ConcreteSpectralAnalyzer::ConcreteSpectralAnalyzer()
    : fft_size_(1024), hop_size_(512), overlap_(0.5f), sample_rate_(44100) {
    // Initialize with Hann window
    window_ = window_hanning(fft_size_);
}

void ConcreteSpectralAnalyzer::set_fft_size(int n_fft) {
    fft_size_ = n_fft;
    hop_size_ = n_fft / 2;  // 50% overlap default
    overlap_ = 0.5f;
    window_ = window_hanning(n_fft);
}

void ConcreteSpectralAnalyzer::set_window(const std::vector<float>& window) {
    window_ = window;
}

void ConcreteSpectralAnalyzer::set_overlap(float ratio) {
    overlap_ = ratio;
    hop_size_ = static_cast<int>(fft_size_ * (1.0f - overlap_));
}

bool ConcreteSpectralAnalyzer::analyze_frame(const float* samples, int num_samples, SpectralFrame& out) {
    // Use first fft_size_ samples (or zero-pad if fewer)
    return analyze_segment(samples, num_samples, out);
}

bool ConcreteSpectralAnalyzer::analyze_buffer(const AudioBuffer& buf, SpectralFrame& out) {
    // Use the first channel's data
    if (buf.num_channels < 1) return false;
    const float* samples = buf.data.data();  // first channel data
    return analyze_segment(samples, buf.size(), out);
}

bool ConcreteSpectralAnalyzer::analyze_segment(const float* samples, int num_available, SpectralFrame& out) {
    int n_fft = fft_size_;

    // Extract n_fft samples (or zero-pad if fewer available)
    std::vector<complex_f> x(n_fft, complex_f(0.0f, 0.0f));

    int samples_to_copy = std::min(num_available, n_fft);
    for (int i = 0; i < samples_to_copy; ++i) {
        x[i] = complex_f(samples[i], 0.0f);
    }
    // Remaining bins stay zero-padded

    // Apply window
    for (int i = 0; i < n_fft; ++i) {
        float w = window_[i];
        x[i] = complex_f(x[i].real() * w, x[i].imag() * w);
    }

    // Forward FFT
    ::fft(x, false);  // use the global fft function

    // Compute magnitude spectrum
    std::vector<float> mag = fft_magnitude(x);

    // Only keep first half (0 to Nyquist): bins 0 through n_fft/2
    int half = n_fft / 2 + 1;
    std::vector<float> mag_half(mag.begin(), mag.begin() + half);

    // Compute power spectrum
    std::vector<float> power = fft_power(x);
    std::vector<float> power_half(power.begin(), power.begin() + half);

    // Normalize by coherent gain for amplitude accuracy
    float cg = window_coherent_gain(window_);
    if (cg > 0.0f) {
        for (auto& m : mag_half) m /= cg;
        for (auto& p : power_half) p /= (cg * cg);
    }

    // Frequency axis
    float freq_res = static_cast<float>(sample_rate_) / static_cast<float>(n_fft);
    out.frame_index = 0;
    out.n_fft = n_fft;
    out.window_factor = cg;

    // Magnitudes (already normalized)
    out.magnitudes.assign(mag_half.begin(), mag_half.end());

    // Phases
    out.phases.resize(half);
    for (int k = 0; k < half; ++k) {
        out.phases[k] = static_cast<float>(std::atan2(x[k].imag(), x[k].real()));
    }

    // Store sample rate for reference
    out.sample_rate = sample_rate_;

    return true;
}

int ConcreteSpectralAnalyzer::freq_bins() const {
    // Number of frequency bins excluding DC (bin 0) and Nyquist (bin n_fft/2)
    // Bins 1 through n_fft/2 - 1 give us (n_fft/2 - 1) bins
    return fft_size_ / 2 - 1;
}

float ConcreteSpectralAnalyzer::max_frequency() const {
    return static_cast<float>(sample_rate_) / 2.0f;
}