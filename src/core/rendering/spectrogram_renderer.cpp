#include "spectrogram_renderer.h"
#include "png_encoder.h"
#include "frequency_scale.h"
#include "thread_pool.h"
#ifdef _WIN32
#include "gpu_spectrogram.h"
#include "d3d11_context.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace Spectral {

// ============================================================================
// dB normalization
// ============================================================================
float SpectrogramRenderer::normalize_db(float db, float db_floor, float db_ceiling) {
    if (!std::isfinite(db)) return 0.0f;
    if (db_ceiling <= db_floor) return 0.0f;
    if (db < db_floor) return 0.0f;
    if (db > db_ceiling) return 1.0f;
    return (db - db_floor) / (db_ceiling - db_floor);
}

// ============================================================================
// Color maps
// ============================================================================
//
// Viridis polynomial fit (van der Walt & Smith, 2015). 6th-order per-channel
// polynomial approximations published by Matt Mitchell / Inigo Quilez; values
// reproduced from a public reference table (e.g. the matplotlib viridis LUT).
// They are NOT measured from raw data — they are a fixed, perceptually
// uniform, color-blind-safe default. t in [0,1] -> (R,G,B) in [0,1].
//
void SpectrogramRenderer::color_viridis(float t, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    // Viridis 256-entry LUT (R, G, B) reproduced from matplotlib's
    // viridis colormap (van der Walt & Smith, 2015). Public domain
    // reference; values taken from the standard published table.
    // 256 rows, 768 bytes.
    static const uint8_t kViridis[256][3] = {
        {0x44, 0x01, 0x54}, {0x44, 0x02, 0x55}, {0x44, 0x03, 0x57}, {0x45, 0x05, 0x58},
        {0x45, 0x06, 0x5A}, {0x45, 0x08, 0x5B}, {0x46, 0x09, 0x5C}, {0x46, 0x0B, 0x5E},
        {0x46, 0x0C, 0x5F}, {0x46, 0x0E, 0x61}, {0x47, 0x0F, 0x62}, {0x47, 0x11, 0x63},
        {0x47, 0x12, 0x65}, {0x47, 0x14, 0x66}, {0x47, 0x15, 0x67}, {0x47, 0x16, 0x69},
        {0x47, 0x18, 0x6A}, {0x48, 0x19, 0x6B}, {0x48, 0x1A, 0x6C}, {0x48, 0x1C, 0x6E},
        {0x48, 0x1D, 0x6F}, {0x48, 0x1E, 0x70}, {0x48, 0x20, 0x71}, {0x48, 0x21, 0x72},
        {0x48, 0x22, 0x73}, {0x48, 0x23, 0x74}, {0x47, 0x25, 0x75}, {0x47, 0x26, 0x76},
        {0x47, 0x27, 0x77}, {0x47, 0x28, 0x78}, {0x47, 0x2A, 0x79}, {0x47, 0x2B, 0x7A},
        {0x47, 0x2C, 0x7B}, {0x46, 0x2D, 0x7C}, {0x46, 0x2F, 0x7C}, {0x46, 0x30, 0x7D},
        {0x46, 0x31, 0x7E}, {0x45, 0x32, 0x7F}, {0x45, 0x34, 0x7F}, {0x45, 0x35, 0x80},
        {0x45, 0x36, 0x81}, {0x44, 0x37, 0x81}, {0x44, 0x39, 0x82}, {0x43, 0x3A, 0x83},
        {0x43, 0x3B, 0x83}, {0x43, 0x3C, 0x84}, {0x42, 0x3D, 0x84}, {0x42, 0x3E, 0x85},
        {0x42, 0x40, 0x85}, {0x41, 0x41, 0x86}, {0x41, 0x42, 0x86}, {0x40, 0x43, 0x87},
        {0x40, 0x44, 0x87}, {0x3F, 0x45, 0x87}, {0x3F, 0x47, 0x88}, {0x3E, 0x48, 0x88},
        {0x3E, 0x49, 0x89}, {0x3D, 0x4A, 0x89}, {0x3D, 0x4B, 0x89}, {0x3D, 0x4C, 0x89},
        {0x3C, 0x4D, 0x8A}, {0x3C, 0x4E, 0x8A}, {0x3B, 0x50, 0x8A}, {0x3B, 0x51, 0x8A},
        {0x3A, 0x52, 0x8B}, {0x3A, 0x53, 0x8B}, {0x39, 0x54, 0x8B}, {0x39, 0x55, 0x8B},
        {0x38, 0x56, 0x8B}, {0x38, 0x57, 0x8C}, {0x37, 0x58, 0x8C}, {0x37, 0x59, 0x8C},
        {0x36, 0x5A, 0x8C}, {0x36, 0x5B, 0x8C}, {0x35, 0x5C, 0x8C}, {0x35, 0x5D, 0x8C},
        {0x34, 0x5E, 0x8D}, {0x34, 0x5F, 0x8D}, {0x33, 0x60, 0x8D}, {0x33, 0x61, 0x8D},
        {0x32, 0x62, 0x8D}, {0x32, 0x63, 0x8D}, {0x31, 0x64, 0x8D}, {0x31, 0x65, 0x8D},
        {0x31, 0x66, 0x8D}, {0x30, 0x67, 0x8D}, {0x30, 0x68, 0x8D}, {0x2F, 0x69, 0x8D},
        {0x2F, 0x6A, 0x8D}, {0x2E, 0x6B, 0x8E}, {0x2E, 0x6C, 0x8E}, {0x2E, 0x6D, 0x8E},
        {0x2D, 0x6E, 0x8E}, {0x2D, 0x6F, 0x8E}, {0x2C, 0x70, 0x8E}, {0x2C, 0x71, 0x8E},
        {0x2C, 0x72, 0x8E}, {0x2B, 0x73, 0x8E}, {0x2B, 0x74, 0x8E}, {0x2A, 0x75, 0x8E},
        {0x2A, 0x76, 0x8E}, {0x2A, 0x77, 0x8E}, {0x29, 0x78, 0x8E}, {0x29, 0x79, 0x8E},
        {0x28, 0x7A, 0x8E}, {0x28, 0x7A, 0x8E}, {0x28, 0x7B, 0x8E}, {0x27, 0x7C, 0x8E},
        {0x27, 0x7D, 0x8E}, {0x27, 0x7E, 0x8E}, {0x26, 0x7F, 0x8E}, {0x26, 0x80, 0x8E},
        {0x26, 0x81, 0x8E}, {0x25, 0x82, 0x8E}, {0x25, 0x83, 0x8D}, {0x24, 0x84, 0x8D},
        {0x24, 0x85, 0x8D}, {0x24, 0x86, 0x8D}, {0x23, 0x87, 0x8D}, {0x23, 0x88, 0x8D},
        {0x23, 0x89, 0x8D}, {0x22, 0x89, 0x8D}, {0x22, 0x8A, 0x8D}, {0x22, 0x8B, 0x8D},
        {0x21, 0x8C, 0x8D}, {0x21, 0x8D, 0x8C}, {0x21, 0x8E, 0x8C}, {0x20, 0x8F, 0x8C},
        {0x20, 0x90, 0x8C}, {0x20, 0x91, 0x8C}, {0x1F, 0x92, 0x8C}, {0x1F, 0x93, 0x8B},
        {0x1F, 0x94, 0x8B}, {0x1F, 0x95, 0x8B}, {0x1F, 0x96, 0x8B}, {0x1E, 0x97, 0x8A},
        {0x1E, 0x98, 0x8A}, {0x1E, 0x99, 0x8A}, {0x1E, 0x99, 0x8A}, {0x1E, 0x9A, 0x89},
        {0x1E, 0x9B, 0x89}, {0x1E, 0x9C, 0x89}, {0x1E, 0x9D, 0x88}, {0x1E, 0x9E, 0x88},
        {0x1E, 0x9F, 0x88}, {0x1E, 0xA0, 0x87}, {0x1F, 0xA1, 0x87}, {0x1F, 0xA2, 0x86},
        {0x1F, 0xA3, 0x86}, {0x20, 0xA4, 0x85}, {0x20, 0xA5, 0x85}, {0x21, 0xA6, 0x85},
        {0x21, 0xA7, 0x84}, {0x22, 0xA7, 0x84}, {0x23, 0xA8, 0x83}, {0x23, 0xA9, 0x82},
        {0x24, 0xAA, 0x82}, {0x25, 0xAB, 0x81}, {0x26, 0xAC, 0x81}, {0x27, 0xAD, 0x80},
        {0x28, 0xAE, 0x7F}, {0x29, 0xAF, 0x7F}, {0x2A, 0xB0, 0x7E}, {0x2B, 0xB1, 0x7D},
        {0x2C, 0xB1, 0x7D}, {0x2E, 0xB2, 0x7C}, {0x2F, 0xB3, 0x7B}, {0x30, 0xB4, 0x7A},
        {0x32, 0xB5, 0x7A}, {0x33, 0xB6, 0x79}, {0x35, 0xB7, 0x78}, {0x36, 0xB8, 0x77},
        {0x38, 0xB9, 0x76}, {0x39, 0xB9, 0x76}, {0x3B, 0xBA, 0x75}, {0x3D, 0xBB, 0x74},
        {0x3E, 0xBC, 0x73}, {0x40, 0xBD, 0x72}, {0x42, 0xBE, 0x71}, {0x44, 0xBE, 0x70},
        {0x45, 0xBF, 0x6F}, {0x47, 0xC0, 0x6E}, {0x49, 0xC1, 0x6D}, {0x4B, 0xC2, 0x6C},
        {0x4D, 0xC2, 0x6B}, {0x4F, 0xC3, 0x69}, {0x51, 0xC4, 0x68}, {0x53, 0xC5, 0x67},
        {0x55, 0xC6, 0x66}, {0x57, 0xC6, 0x65}, {0x59, 0xC7, 0x64}, {0x5B, 0xC8, 0x62},
        {0x5E, 0xC9, 0x61}, {0x60, 0xC9, 0x60}, {0x62, 0xCA, 0x5F}, {0x64, 0xCB, 0x5D},
        {0x67, 0xCC, 0x5C}, {0x69, 0xCC, 0x5B}, {0x6B, 0xCD, 0x59}, {0x6D, 0xCE, 0x58},
        {0x70, 0xCE, 0x56}, {0x72, 0xCF, 0x55}, {0x74, 0xD0, 0x54}, {0x77, 0xD0, 0x52},
        {0x79, 0xD1, 0x51}, {0x7C, 0xD2, 0x4F}, {0x7E, 0xD2, 0x4E}, {0x81, 0xD3, 0x4C},
        {0x83, 0xD3, 0x4B}, {0x86, 0xD4, 0x49}, {0x88, 0xD5, 0x47}, {0x8B, 0xD5, 0x46},
        {0x8D, 0xD6, 0x44}, {0x90, 0xD6, 0x43}, {0x92, 0xD7, 0x41}, {0x95, 0xD7, 0x3F},
        {0x97, 0xD8, 0x3E}, {0x9A, 0xD8, 0x3C}, {0x9D, 0xD9, 0x3A}, {0x9F, 0xD9, 0x38},
        {0xA2, 0xDA, 0x37}, {0xA5, 0xDA, 0x35}, {0xA7, 0xDB, 0x33}, {0xAA, 0xDB, 0x32},
        {0xAD, 0xDC, 0x30}, {0xAF, 0xDC, 0x2E}, {0xB2, 0xDD, 0x2C}, {0xB5, 0xDD, 0x2B},
        {0xB7, 0xDD, 0x29}, {0xBA, 0xDE, 0x27}, {0xBD, 0xDE, 0x26}, {0xBF, 0xDF, 0x24},
        {0xC2, 0xDF, 0x22}, {0xC5, 0xDF, 0x21}, {0xC7, 0xE0, 0x1F}, {0xCA, 0xE0, 0x1E},
        {0xCD, 0xE0, 0x1D}, {0xCF, 0xE1, 0x1C}, {0xD2, 0xE1, 0x1B}, {0xD4, 0xE1, 0x1A},
        {0xD7, 0xE2, 0x19}, {0xDA, 0xE2, 0x18}, {0xDC, 0xE2, 0x18}, {0xDF, 0xE3, 0x18},
        {0xE1, 0xE3, 0x18}, {0xE4, 0xE3, 0x18}, {0xE7, 0xE4, 0x19}, {0xE9, 0xE4, 0x19},
        {0xEC, 0xE4, 0x1A}, {0xEE, 0xE5, 0x1B}, {0xF1, 0xE5, 0x1C}, {0xF3, 0xE5, 0x1E},
        {0xF6, 0xE6, 0x1F}, {0xF8, 0xE6, 0x21}, {0xFA, 0xE6, 0x22}, {0xFD, 0xE7, 0x24}
    };
    // Map t in [0,1] to LUT index 0..255 with linear interpolation.
    const float idx_f = t * 255.0f;
    const int idx0 = static_cast<int>(idx_f);
    const int idx1 = (idx0 < 255) ? idx0 + 1 : 255;
    const float f = idx_f - idx0;
    r = static_cast<uint8_t>(kViridis[idx0][0] + f * (kViridis[idx1][0] - kViridis[idx0][0]));
    g = static_cast<uint8_t>(kViridis[idx0][1] + f * (kViridis[idx1][1] - kViridis[idx0][1]));
    b = static_cast<uint8_t>(kViridis[idx0][2] + f * (kViridis[idx1][2] - kViridis[idx0][2]));
}

// Heat: black -> dark red -> red -> orange -> yellow -> white.
void SpectrogramRenderer::color_heat(float t, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    // Three-segment piecewise linear ramp.
    if (t < 0.33f) {
        const float u = t / 0.33f;
        r = static_cast<uint8_t>(u * 180.0f);
        g = 0;
        b = 0;
    } else if (t < 0.66f) {
        const float u = (t - 0.33f) / 0.33f;
        r = static_cast<uint8_t>(180.0f + u * 75.0f);
        g = static_cast<uint8_t>(u * 60.0f);
        b = 0;
    } else {
        const float u = (t - 0.66f) / 0.34f;
        r = 255;
        g = static_cast<uint8_t>(60.0f + u * 195.0f);
        b = static_cast<uint8_t>(u * 220.0f);
    }
}

void SpectrogramRenderer::color_map(ColorMap cm, float t,
                                    uint8_t& r, uint8_t& g, uint8_t& b) {
    switch (cm) {
        case ColorMap::Heat: color_heat(t, r, g, b); break;
        case ColorMap::Viridis:
        default:            color_viridis(t, r, g, b); break;
    }
}

// ============================================================================
// Helpers
// ============================================================================
namespace {

// Map a frequency value to a row index in [0, height-1].
// row 0 is top (highest frequency when using the standard spectrogram
// convention "high freq at top"). Bottom row = freq_min.
struct FreqMapper {
    int scale_enum = 1;  // FrequencyScale value
    float fmin = 20.0f;
    float fmax = 20000.0f;
    int height = 0;
    float cqt_center = 0.0f;
    float cqt_q = 0.0f;

    float row_for(float freq) const {
        if (height <= 0) return 0.0f;
        float t = hz_to_unit(freq, scale_enum, fmin, fmax, cqt_center, cqt_q);
        return (1.0f - t) * static_cast<float>(height - 1);
    }
};

// Time -> column index in [0, width-1].
struct TimeMapper {
    double t0 = 0.0;
    double t1 = 0.0;
    int width = 0;

    // Returns the fractional column.
    double col_for(double t) const {
        if (width <= 0 || t1 <= t0) return 0.0;
        double u = (t - t0) / (t1 - t0);
        if (u < 0.0) u = 0.0;
        if (u > 1.0) u = 1.0;
        return u * static_cast<double>(width - 1);
    }
};

// Sanitize a magnitude value to dB. Silences -> floor. NaN/inf -> floor.
float mag_to_db_safe(float mag, float db_floor, float reference) {
    if (!std::isfinite(mag) || mag <= 0.0f) return db_floor;
    if (reference <= 0.0f) reference = 1.0f;
    const float v = 20.0f * std::log10(mag / reference);
    if (!std::isfinite(v)) return db_floor;
    return v;
}

// Linear interpolation between two float samples.
inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

} // namespace

// ============================================================================
// Render
// ============================================================================
RenderError SpectrogramRenderer::render(const SpectralDataset& dataset,
                                        RGBAImage& out,
                                        const std::atomic<bool>* cancel) const {
    out.clear();

    if (cfg_.width <= 0 || cfg_.height <= 0) return RenderError::InvalidDimensions;
    if (dataset.frame_count() <= 0 || dataset.num_frequency_bins() <= 0) {
        return RenderError::EmptyDataset;
    }

    const float nyquist = (cfg_.freq_max_hz > 0.0f)
                              ? cfg_.freq_max_hz
                              : dataset.nyquist_frequency();
    const float fmin = std::max(1.0f, cfg_.freq_min_hz);
    const float fmax = nyquist;
    if (fmax <= fmin) return RenderError::InvalidFrequencyRange;

    const int W = cfg_.width;
    const int H = cfg_.height;
    out.width = W;
    out.height = H;
    out.pixels.assign(static_cast<size_t>(W) * H * 4, 0);

    // Fill background
    for (int y = 0; y < H; ++y) {
        uint8_t* row = &out.pixels[static_cast<size_t>(y) * W * 4];
        for (int x = 0; x < W; ++x) {
            row[x*4+0] = cfg_.bg_r;
            row[x*4+1] = cfg_.bg_g;
            row[x*4+2] = cfg_.bg_b;
            row[x*4+3] = cfg_.bg_a;
        }
    }

    const FreqMapper fm{static_cast<int>(cfg_.freq_scale),
                        fmin, fmax, H,
                        cfg_.cqt_center_hz, cfg_.cqt_q};
    const TimeMapper tm{0.0, dataset.total_duration(), W};

    const int Nf = dataset.frame_count();
    const int Nk = dataset.num_frequency_bins();
    const float reference = dataset.normalization_info().reference_amplitude;

    // Check if any frame has reassigned coordinates
    bool has_reassigned = false;
    for (int fi = 0; fi < Nf && !has_reassigned; ++fi) {
        has_reassigned = !dataset.frame(fi).reassigned_freqs.empty();
    }

    if (has_reassigned) {
        // Scatter mode: place each bin's energy at its reassigned position.
        // ponytail: parallelize by frame — frames write to non-overlapping pixels.
        ThreadPool pool;
        const int Nthreads = pool.size();
        const int frames_per = (Nf + Nthreads - 1) / Nthreads;
        std::vector<std::thread> threads;
        threads.reserve(Nthreads);

        for (int t = 0; t < Nthreads; ++t) {
            int fi_lo = t * frames_per;
            int fi_hi = std::min(fi_lo + frames_per, Nf);
            threads.emplace_back([&, fi_lo, fi_hi]() {
                for (int fi = fi_lo; fi < fi_hi; ++fi) {
                    // Row/frame-boundary cancellation: threads finish the
                    // current frame, then exit; the partial image is
                    // discarded after the join below.
                    if (cancel && cancel->load(std::memory_order_acquire)) break;
                    const auto& f0 = dataset.frame(fi);
                    const auto* f1_ptr = (fi + 1 < Nf) ? &dataset.frame(fi + 1) : nullptr;
                    const double t0 = f0.timestamp;
                    const double t1 = f1_ptr ? f1_ptr->timestamp
                                             : (t0 + dataset.frame_duration());
                    const int Nk_f = static_cast<int>(f0.reassigned_freqs.size());
                    if (Nk_f <= 0) continue;

                    for (int k = 0; k < Nk_f; ++k) {
                        float mag = f0.magnitudes[static_cast<size_t>(k)];
                        if (mag <= 0.0f) continue;

                        float rfreq = f0.reassigned_freqs[static_cast<size_t>(k)];
                        if (rfreq < fmin || rfreq > fmax) continue;
                        float row_f = fm.row_for(rfreq);
                        int row = static_cast<int>(std::round(row_f));
                        if (row < 0 || row >= H) continue;

                        double rtime = t0 + static_cast<double>(f0.reassigned_times[static_cast<size_t>(k)]);
                        double col_d = tm.col_for(rtime);
                        int col = static_cast<int>(std::round(col_d));
                        if (col < 0 || col >= W) continue;

                        float db = mag_to_db_safe(mag, cfg_.db_floor, reference);
                        const float t_norm = normalize_db(db, cfg_.db_floor, cfg_.db_ceiling);
                        uint8_t r, g, b;
                        color_map(cfg_.color_map, t_norm, r, g, b);
                        uint8_t* p = &out.pixels[(static_cast<size_t>(row) * W + col) * 4];
                        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    } else {
        // Gather mode: conventional STFT rendering.
        // ponytail: parallelize by output rows — each row writes non-overlapping pixels.
        auto sample_db = [&](const SpectralFrame& fr, int bin) -> float {
            if (bin < 0 || bin >= Nk) return cfg_.db_floor;
            const float m = fr.magnitudes[bin];
            return mag_to_db_safe(m, cfg_.db_floor, reference);
        };

        // Precompute column ranges per frame (shared across threads, read-only).
        struct FrameCol {
            int xa, xb;
            double c0, c1;
        };
        std::vector<FrameCol> frame_cols(Nf);
        for (int fi = 0; fi < Nf; ++fi) {
            const auto& f0 = dataset.frame(fi);
            const auto* f1_ptr = (fi + 1 < Nf) ? &dataset.frame(fi + 1) : nullptr;
            const double t0 = f0.timestamp;
            const double t1 = f1_ptr ? f1_ptr->timestamp
                                     : (t0 + dataset.frame_duration());
            const double c0 = tm.col_for(t0);
            const double c1 = tm.col_for(t1);
            const int x_lo = static_cast<int>(std::floor(std::min(c0, c1)));
            const int x_hi = static_cast<int>(std::ceil (std::max(c0, c1)));
            frame_cols[fi] = {std::max(0, x_lo), std::min(W - 1, x_hi), c0, c1};
        }

        ThreadPool pool;
        const int Nthreads = pool.size();
        const int rows_per = (H + Nthreads - 1) / Nthreads;
        std::vector<std::thread> threads;
        threads.reserve(Nthreads);

        for (int t = 0; t < Nthreads; ++t) {
            int y_lo = t * rows_per;
            int y_hi = std::min(y_lo + rows_per, H);
            threads.emplace_back([&, y_lo, y_hi]() {
                for (int y = y_lo; y < y_hi; ++y) {
                    if (cancel && cancel->load(std::memory_order_acquire)) break;
                    const double low_frac = 1.0 - static_cast<double>(y) / (H - 1);
                    float freq = unit_to_hz(static_cast<float>(low_frac),
                                            static_cast<int>(cfg_.freq_scale),
                                            fmin, fmax,
                                            cfg_.cqt_center_hz, cfg_.cqt_q);
                    const float bin_f = static_cast<float>(freq) /
                                        std::max(1.0f, dataset.frequency_resolution());
                    int bin_lo = static_cast<int>(std::floor(bin_f));
                    int bin_hi = bin_lo + 1;
                    if (bin_lo < 0) bin_lo = 0;
                    if (bin_lo >= Nk) bin_lo = Nk - 1;
                    if (bin_hi < 0) bin_hi = 0;
                    if (bin_hi >= Nk) bin_hi = Nk - 1;
                    float bin_t = bin_f - std::floor(bin_f);
                    if (bin_t < 0) bin_t = 0;
                    if (bin_t > 1) bin_t = 1;

                    for (int fi = 0; fi < Nf; ++fi) {
                        const auto& fc = frame_cols[fi];
                        const auto& f0 = dataset.frame(fi);
                        const auto* f1_ptr = (fi + 1 < Nf) ? &dataset.frame(fi + 1) : nullptr;
                        const double c0 = fc.c0;
                        const double c1 = fc.c1;

                        for (int x = fc.xa; x <= fc.xb; ++x) {
                            double wt = 0.0;
                            if (c1 > c0) {
                                wt = ((double)x - c0) / (c1 - c0);
                                if (wt < 0.0) wt = 0.0;
                                if (wt > 1.0) wt = 1.0;
                            }

                            float db;
                            if (cfg_.interpolation == Interpolation::Nearest) {
                                db = sample_db(f0, bin_lo);
                            } else {
                                const float db_lo_now = sample_db(f0, bin_lo);
                                const float db_hi_now = sample_db(f0, bin_hi);
                                const float db_freq_now = lerp(db_lo_now, db_hi_now, bin_t);
                                if (f1_ptr == nullptr || wt <= 0.0) {
                                    db = db_freq_now;
                                } else {
                                    const float db_lo_next = sample_db(*f1_ptr, bin_lo);
                                    const float db_hi_next = sample_db(*f1_ptr, bin_hi);
                                    const float db_freq_next = lerp(db_lo_next, db_hi_next, bin_t);
                                    db = lerp(db_freq_now, db_freq_next, static_cast<float>(wt));
                                }
                            }

                            const float t = normalize_db(db, cfg_.db_floor, cfg_.db_ceiling);
                            uint8_t r, g, b;
                            color_map(cfg_.color_map, t, r, g, b);
                            uint8_t* p = &out.pixels[(static_cast<size_t>(y) * W + x) * 4];
                            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
                        }
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    if (cancel && cancel->load(std::memory_order_acquire)) {
        out.clear();
        return RenderError::Cancelled;
    }
    return RenderError::Ok;
}

RenderError SpectrogramRenderer::render_to_png(const SpectralDataset& dataset,
                                               const std::string& png_path,
                                               const std::atomic<bool>* cancel) const {
    RGBAImage img;
    RenderError err = render(dataset, img, cancel);
    if (err != RenderError::Ok) return err;
    if (!PNGEncoder::write_rgba(png_path, img.width, img.height, img.pixels.data())) {
        return RenderError::InvalidDimensions;
    }
    return RenderError::Ok;
}

RenderError SpectrogramRenderer::render_gpu(const SpectralDataset& dataset,
                                            RGBAImage& out,
                                            const std::atomic<bool>* cancel) const {
#ifdef _WIN32
    if (cancel && cancel->load(std::memory_order_acquire)) {
        out.clear();
        return RenderError::Cancelled;
    }
    // ponytail: static context — device init once, reused across renders
    static D3D11Context ctx;
    static GpuSpectrogram gpu(ctx);
    if (!gpu.is_available()) return render(dataset, out, cancel);  // CPU fallback
    RenderError err = gpu.render(dataset, cfg_, out);
    if (err != RenderError::Ok) return render(dataset, out, cancel);  // GPU fail → CPU
    if (cancel && cancel->load(std::memory_order_acquire)) {
        out.clear();
        return RenderError::Cancelled;
    }
    return RenderError::Ok;
#else
    return render(dataset, out, cancel);
#endif
}

} // namespace Spectral
