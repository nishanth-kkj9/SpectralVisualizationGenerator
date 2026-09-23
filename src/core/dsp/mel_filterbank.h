#pragma once

// Phase 9 — real triangular Mel filterbank (representation, not display).
//
// Mel scale (project formula, shared with the display mapper):
//   mel(f) = 2595 * log10(1 + f/700),  mel(0) = 0
//   hz(m)  = 700 * (10^(m/2595) - 1)
// Filterbank: `bands` triangular filters with centers uniformly spaced in
// Mel between mel(fmin) and mel(fmax), edges at the neighboring centers
// (first left edge = fmin, last right edge = fmax). Weights map FFT bins
// (uniform Hz grid) by linear interpolation in Hz.
//
// Weight normalization (three DISTINCT, precisely defined modes):
//   None   — raw peak-1 triangles, no normalization.
//   Slaney — each filter divided by half its Hz width (right-left)/2,
//            i.e. by its analytic triangle area: unit area in Hz
//            (librosa-style width normalization, enorm = 2/width).
//   Area   — each filter divided by its discrete weight sum (unit sum).
// Slaney and Area differ per filter shape; None is unnormalized.
//
// Aggregation contract: the filterbank consumes the STFT contract's
// one-sided amplitude-corrected POWER per FFT bin (interior bins doubled,
// DC/Nyquist single, scale 1/(N*cg)) and produces Mel ENERGY per band
// (weighted power accumulation). Callers store amplitude (sqrt) in
// magnitudes and energy in power, preserving the power == mag^2 field
// invariant everywhere.

#include "fft.h"
#include "frequency_scale.h"
#include "../spectral/representation.h"

#include <cmath>
#include <string>
#include <vector>

namespace Spectral {

struct MelFilterbankConfig {
    int sample_rate = 0;
    int fft_size = 0;
    float fmin_hz = 0.0f;
    float fmax_hz = 0.0f;  // <= 0 = Nyquist (resolved at build time)
    int bands = 0;
    RepresentationNorm norm = RepresentationNorm::Slaney;
};

// Built filterbank: row-major weights [bands x fft_bins], explicit edges
// and centers in Hz. Immutable after successful build().
class MelFilterbank {
public:
    MelFilterbank() = default;

    // Build and validate. False (with reason) on any impossible geometry:
    // non-positive rate/size, bad range, fmax above Nyquist, bad band
    // count, more bands than FFT bins, or any zero-weight filter.
    bool build(const MelFilterbankConfig& cfg, std::string& error);

    bool valid() const { return !weights_.empty(); }
    int bands() const { return bands_; }
    int fft_bins() const { return fft_bins_; }
    float fmin_hz() const { return fmin_; }
    float fmax_hz() const { return fmax_; }
    RepresentationNorm norm() const { return norm_; }
    const std::vector<float>& centers_hz() const { return centers_; }
    // Dense row-major weights (bands x fft_bins); row b covers only bins
    // inside (left[b], right[b]), zeros elsewhere.
    const std::vector<float>& weights() const { return weights_; }
    const std::vector<float>& left_hz() const { return left_; }
    const std::vector<float>& right_hz() const { return right_; }

    // Weighted power accumulation for one frame. power must hold fft_bins
    // entries; out must hold bands() entries. No allocation.
    void apply(const float* power, float* out) const;

private:
    int bands_ = 0;
    int fft_bins_ = 0;
    float fmin_ = 0.0f;
    float fmax_ = 0.0f;
    RepresentationNorm norm_ = RepresentationNorm::None;
    std::vector<float> centers_;
    std::vector<float> left_;
    std::vector<float> right_;
    std::vector<float> weights_;
};

inline bool MelFilterbank::build(const MelFilterbankConfig& cfg, std::string& error) {
    *this = MelFilterbank();
    if (cfg.sample_rate <= 0) {
        error = "mel: sample_rate must be > 0";
        return false;
    }
    if (!is_valid_fft_size(cfg.fft_size)) {
        error = "mel: fft_size must be a power of two >= 2";
        return false;
    }
    const float nyquist = static_cast<float>(cfg.sample_rate) / 2.0f;
    const float fmax = (cfg.fmax_hz <= 0.0f) ? nyquist : cfg.fmax_hz;
    if (!(cfg.fmin_hz >= 0.0f) || !std::isfinite(cfg.fmin_hz)) {
        error = "mel: fmin_hz must be finite and >= 0";
        return false;
    }
    if (!std::isfinite(fmax) || fmax <= cfg.fmin_hz) {
        error = "mel: fmax_hz must be finite and > fmin_hz";
        return false;
    }
    if (fmax > nyquist) {
        error = "mel: fmax_hz must be <= Nyquist";
        return false;
    }
    const int bins = cfg.fft_size / 2 + 1;
    if (cfg.bands <= 0) {
        error = "mel: bands must be > 0";
        return false;
    }
    if (cfg.bands > bins) {
        // More triangular filters than feeding FFT bins cannot add
        // information and forces degenerate (duplicate/empty) filters.
        error = "mel: bands must be <= fft bins";
        return false;
    }
    if (cfg.norm != RepresentationNorm::None && cfg.norm != RepresentationNorm::Slaney &&
        cfg.norm != RepresentationNorm::Area) {
        error = "mel: unknown normalization";
        return false;
    }
    // Mel-spaced edges: bands+2 points from mel(fmin) to mel(fmax).
    const float m_lo = hz_to_mel(cfg.fmin_hz);
    const float m_hi = hz_to_mel(fmax);
    if (!(m_hi > m_lo)) {
        error = "mel: degenerate mel range";
        return false;
    }
    bands_ = cfg.bands;
    std::vector<float> edges(static_cast<size_t>(bands_) + 2);
    for (int i = 0; i < bands_ + 2; ++i) {
        const float m = m_lo + (m_hi - m_lo) * i / (bands_ + 1);
        edges[i] = mel_to_hz(m);
    }
    centers_.resize(bands_);
    left_.resize(bands_);
    right_.resize(bands_);
    weights_.assign(static_cast<size_t>(bands_) * bins, 0.0f);
    const float bin_hz = static_cast<float>(cfg.sample_rate) / cfg.fft_size;
    for (int b = 0; b < bands_; ++b) {
        const float left = (b == 0) ? cfg.fmin_hz : edges[b];
        const float center = edges[b + 1];
        const float right = (b == bands_ - 1) ? fmax : edges[b + 2];
        if (!(left < center && center < right)) {
            error = "mel: degenerate filter geometry";
            *this = MelFilterbank();
            return false;
        }
        centers_[b] = center;
        left_[b] = left;
        right_[b] = right;
        float* w = weights_.data() + static_cast<size_t>(b) * bins;
        for (int k = 0; k < bins; ++k) {
            const float f = k * bin_hz;
            if (f <= left || f >= right) continue;
            w[k] = (f <= center) ? (f - left) / (center - left) : (right - f) / (right - center);
        }
        float norm_div = 1.0f;
        if (cfg.norm == RepresentationNorm::Slaney) {
            norm_div = (right - left) / 2.0f;
        } else if (cfg.norm == RepresentationNorm::Area) {
            double sum = 0.0;
            for (int k = 0; k < bins; ++k) sum += w[k];
            norm_div = static_cast<float>(sum);
        }
        if (!(norm_div > 0.0f)) {
            error = "mel: zero-area filter (reduce bands or widen range)";
            *this = MelFilterbank();
            return false;
        }
        if (norm_div != 1.0f) {
            for (int k = 0; k < bins; ++k) w[k] /= norm_div;
        }
        double check = 0.0;
        for (int k = 0; k < bins; ++k) check += w[k];
        if (!(check > 0.0)) {
            error = "mel: zero-weight filter (reduce bands or widen range)";
            *this = MelFilterbank();
            return false;
        }
    }
    fft_bins_ = bins;
    fmin_ = cfg.fmin_hz;
    fmax_ = fmax;
    norm_ = cfg.norm;
    return true;
}

inline void MelFilterbank::apply(const float* power, float* out) const {
    for (int b = 0; b < bands_; ++b) {
        const float* w = weights_.data() + static_cast<size_t>(b) * fft_bins_;
        double acc = 0.0;
        for (int k = 0; k < fft_bins_; ++k) {
            const float wk = w[k];
            if (wk != 0.0f) acc += static_cast<double>(wk) * power[k];
        }
        out[b] = static_cast<float>(acc);
    }
}

} // namespace Spectral
