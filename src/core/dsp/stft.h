#pragma once

// S4 — the one authoritative STFT frame math in production.
// Normalization convention (documented, tested, see docs/dsp/fft-stft.md):
//   mag[k] = |X[k]| / (N * cg), one-sided (bins 1..N/2-1 doubled),
//   power  = mag^2, phase = atan2(im, re).
// So an exact-bin unit sinusoid reads magnitude 1.0 regardless of N or
// window. DC and Nyquist bins are NOT doubled (single-sided by nature).
// Callers needing the raw complex spectrum (reassignment) get it too.

#include "fft.h"

#include <cmath>
#include <string>
#include <vector>

namespace Spectral {

// Frame policy: floor((total - n_fft) / hop) + 1; 0 when total < n_fft.
// Frame f starts at sample f * hop; timestamps are start / sample_rate.
inline int stft_frame_count(int total, int n_fft, int hop) {
    if (n_fft <= 0 || hop <= 0 || total < n_fft) return 0;
    return (total - n_fft) / hop + 1;
}

struct StftFrame {
    std::vector<float> magnitudes;  // n_fft/2+1, amplitude-corrected
    std::vector<float> phases;      // n_fft/2+1
    std::vector<float> power;       // n_fft/2+1, mag^2
    std::vector<complex_f> spectrum;  // raw complex FFT (length n_fft)
    double timestamp = 0.0;           // frame start, seconds
    int frame_index = 0;
};

// One windowed frame. Returns false (outputs cleared) on bad params.
// window.size() must equal n_fft; cg = window_coherent_gain(window).
inline bool stft_frame(const float* audio, int total, int start, int n_fft,
                       int sample_rate, const std::vector<float>& window,
                       float cg, StftFrame& out) {
    out = StftFrame{};
    if (!audio || total < n_fft || start < 0 || start + n_fft > total) return false;
    if (!is_valid_fft_size(n_fft)) return false;
    if (static_cast<int>(window.size()) != n_fft) return false;
    if (!(cg > 0.0f) || sample_rate <= 0) return false;

    const int bins = n_fft / 2 + 1;
    std::vector<complex_f> buf(static_cast<size_t>(n_fft));
    for (int j = 0; j < n_fft; ++j)
        buf[static_cast<size_t>(j)] =
            complex_f(audio[start + j] * window[static_cast<size_t>(j)], 0.0f);
    fft(buf, false);

    out.magnitudes.resize(static_cast<size_t>(bins));
    out.phases.resize(static_cast<size_t>(bins));
    out.power.resize(static_cast<size_t>(bins));
    const float norm = 1.0f / (static_cast<float>(n_fft) * cg);
    for (int k = 0; k < bins; ++k) {
        const float re = buf[static_cast<size_t>(k)].real();
        const float im = buf[static_cast<size_t>(k)].imag();
        float mag = std::sqrt(re * re + im * im) * norm;
        // One-sided spectrum: interior bins carry both halves.
        if (k > 0 && k < n_fft / 2) mag *= 2.0f;
        out.magnitudes[static_cast<size_t>(k)] = mag;
        out.phases[static_cast<size_t>(k)] = std::atan2(im, re);
        out.power[static_cast<size_t>(k)] = mag * mag;
    }
    out.spectrum = std::move(buf);
    out.timestamp = static_cast<double>(start) / static_cast<double>(sample_rate);
    return true;
}

// All frames over [0, total). Empty vector when params admit no frames.
inline std::vector<StftFrame> stft_all(const float* audio, int total, int n_fft,
                                       int hop, int sample_rate,
                                       const std::vector<float>& window, float cg) {
    std::vector<StftFrame> frames;
    const int count = stft_frame_count(total, n_fft, hop);
    for (int f = 0; f < count; ++f) {
        StftFrame sf;
        sf.frame_index = f;
        if (!stft_frame(audio, total, f * hop, n_fft, sample_rate, window, cg, sf)) break;
        sf.frame_index = f;
        frames.push_back(std::move(sf));
    }
    return frames;
}

// Window selector shared by pipeline/tests. Unknown names yield an empty
// vector (fail-closed; stft_frame rejects it). No Hann fallback.
inline std::vector<float> stft_window(const std::string& type, int n) {
    if (type == "hamming") return window_hamming(n);
    if (type == "blackman") return window_blackman(n);
    if (type == "rectangular") return window_rectangular(n);
    if (type == "hann") return window_hann(n);
    return {};
}

} // namespace Spectral
