#pragma once

// Phase 11 — Perceptual frequency scale conversions.
// Pure functions, no state, no deps beyond <cmath>.

#include <cmath>
#include <algorithm>

namespace Spectral {

// ---- Mel scale (Stevens, Volkmann, Newman 1937) ----
inline float hz_to_mel(float hz) {
    if (hz <= 0.0f) return 0.0f;
    return 2595.0f * std::log10(1.0f + hz / 700.0f);
}
inline float mel_to_hz(float mel) {
    if (mel <= 0.0f) return 0.0f;
    return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
}

// ---- Bark scale (Zwicker & Terhardt 1980) ----
inline float hz_to_bark(float hz) {
    if (hz <= 0.0f) return 0.0f;
    return 13.0f * std::atan(0.00076f * hz)
         + 3.5f * std::atan(hz * hz / (7500.0f * 7500.0f));
}
inline float bark_to_hz(float bark) {
    if (bark <= 0.0f) return 0.0f;
    float lo = 0.0f, hi = 24000.0f;
    for (int i = 0; i < 24; ++i) {
        float mid = (lo + hi) * 0.5f;
        if (hz_to_bark(mid) < bark) lo = mid;
        else hi = mid;
    }
    return (lo + hi) * 0.5f;
}

// ---- ERB scale (Glasberg & Moore 1990) ----
inline float hz_to_erb(float hz) {
    if (hz <= 0.0f) return 0.0f;
    return 24.7f * (4.37f * hz / 1000.0f + 1.0f);
}
inline float erb_to_hz(float erb) {
    if (erb <= 0.0f) return 0.0f;
    return (erb / 24.7f - 1.0f) * 1000.0f / 4.37f;
}

// ---- CQT bin index (Schörkhuber 2010) ----
inline float hz_to_cqt_bin(float hz, float f_center, float Q) {
    if (hz <= 0.0f || f_center <= 0.0f || Q <= 0.0f) return 0.0f;
    return Q * std::log2(hz / f_center);
}

// ---- Unified normalizer: Hz → [0, 1] ----
inline float hz_to_unit(float hz, int scale_enum,
                        float fmin, float fmax,
                        float cqt_center = 0.0f, float cqt_q = 0.0f) {
    if (fmax <= fmin || hz < 0.0f) return 0.0f;
    float t = 0.0f;
    switch (scale_enum) {
        case 0: // Linear
            t = (hz - fmin) / (fmax - fmin);
            break;
        case 1: { // Logarithmic
            const float lo = std::log10(std::max(fmin, 1.0f));
            const float hi = std::log10(std::max(fmax, 1.0f));
            const float lf = std::log10(std::max(hz, 1.0f));
            t = (hi > lo) ? (lf - lo) / (hi - lo) : 0.0f;
            break;
        }
        case 2: { // Mel
            float mmin = hz_to_mel(fmin);
            float mmax = hz_to_mel(fmax);
            t = (mmax > mmin) ? (hz_to_mel(hz) - mmin) / (mmax - mmin) : 0.0f;
            break;
        }
        case 3: { // Bark
            float bmin = hz_to_bark(fmin);
            float bmax = hz_to_bark(fmax);
            t = (bmax > bmin) ? (hz_to_bark(hz) - bmin) / (bmax - bmin) : 0.0f;
            break;
        }
        case 4: { // ERB
            float emin = hz_to_erb(fmin);
            float emax = hz_to_erb(fmax);
            t = (emax > emin) ? (hz_to_erb(hz) - emin) / (emax - emin) : 0.0f;
            break;
        }
        case 5: { // CQT
            if (cqt_center <= 0.0f || cqt_q <= 0.0f) {
                const float lo = std::log10(std::max(fmin, 1.0f));
                const float hi = std::log10(std::max(fmax, 1.0f));
                const float lf = std::log10(std::max(hz, 1.0f));
                t = (hi > lo) ? (lf - lo) / (hi - lo) : 0.0f;
            } else {
                float bmin = hz_to_cqt_bin(fmin, cqt_center, cqt_q);
                float bmax = hz_to_cqt_bin(fmax, cqt_center, cqt_q);
                float bcur = hz_to_cqt_bin(hz, cqt_center, cqt_q);
                t = (bmax > bmin) ? (bcur - bmin) / (bmax - bmin) : 0.0f;
            }
            break;
        }
        default:
            t = 0.0f;
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

// Inverse of hz_to_unit: [0,1] → Hz via bisection.
inline float unit_to_hz(float u, int scale_enum,
                        float fmin, float fmax,
                        float cqt_center = 0.0f, float cqt_q = 0.0f) {
    if (u <= 0.0f) return fmin;
    if (u >= 1.0f) return fmax;
    float lo = fmin, hi = fmax;
    for (int i = 0; i < 40; ++i) {
        float mid = (lo + hi) * 0.5f;
        float umid = hz_to_unit(mid, scale_enum, fmin, fmax, cqt_center, cqt_q);
        if (umid < u) lo = mid;
        else hi = mid;
    }
    return (lo + hi) * 0.5f;
}

} // namespace Spectral
