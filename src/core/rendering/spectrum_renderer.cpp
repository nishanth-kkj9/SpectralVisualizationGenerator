#include "spectrum_renderer.h"
#include "png_encoder.h"
#include "spectrogram_renderer.h"  // for color_viridis / color_heat
#include "frequency_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace Spectral {

namespace {

// ============================================================================
// Minimal 5x7 bitmap font for axis labels (deterministic, no font dependency).
// Only the characters needed for axis labels are defined; unknowns render as '?'.
// Each glyph is 5 columns x 7 rows. Bit per pixel (MSB = top-left).
// ============================================================================
struct Glyph { uint8_t rows[7]; };

constexpr Glyph kFont[128] = {
    // 0x20 ' '
    {{0,0,0,0,0,0,0}},
    // 0x21 '!'
    {{0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    // 0x22 '"'
    {{0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}},
    // 0x23 '#'
    {{0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}},
    // 0x24 '$'
    {{0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}},
    // 0x25 '%'
    {{0x18,0x19,0x02,0x04,0x08,0x13,0x03}},
    // 0x26 '&'
    {{0x08,0x14,0x14,0x08,0x15,0x12,0x0D}},
    // 0x27 '\''
    {{0x0C,0x04,0x08,0x00,0x00,0x00,0x00}},
    // 0x28 '('
    {{0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    // 0x29 ')'
    {{0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
    // 0x2A '*'
    {{0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00}},
    // 0x2B '+'
    {{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    // 0x2C ','
    {{0x00,0x00,0x00,0x00,0x0C,0x04,0x08}},
    // 0x2D '-'
    {{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    // 0x2E '.'
    {{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    // 0x2F '/'
    {{0x00,0x01,0x02,0x04,0x08,0x10,0x00}},
    // 0x30 '0'
    {{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    // 0x31 '1'
    {{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    // 0x32 '2'
    {{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    // 0x33 '3'
    {{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    // 0x34 '4'
    {{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    // 0x35 '5'
    {{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    // 0x36 '6'
    {{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    // 0x37 '7'
    {{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    // 0x38 '8'
    {{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    // 0x39 '9'
    {{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    // 0x3A ':'
    {{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
    // 0x3B ';'
    {{0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08}},
    // 0x3C '<'
    {{0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
    // 0x3D '='
    {{0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}},
    // 0x3E '>'
    {{0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
    // 0x3F '?'
    {{0x0E,0x11,0x01,0x02,0x04,0x00,0x04}},
    // 0x40 '@'
    {{0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}},
    // 0x41 'A'
    {{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    // 0x42 'B'
    {{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    // 0x43 'C'
    {{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    // 0x44 'D'
    {{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    // 0x45 'E'
    {{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    // 0x46 'F'
    {{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    // 0x47 'G'
    {{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    // 0x48 'H'
    {{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    // 0x49 'I'
    {{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    // 0x4A 'J'
    {{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    // 0x4B 'K'
    {{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    // 0x4C 'L'
    {{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    // 0x4D 'M'
    {{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    // 0x4E 'N'
    {{0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
    // 0x4F 'O'
    {{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    // 0x50 'P'
    {{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    // 0x51 'Q'
    {{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    // 0x52 'R'
    {{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    // 0x53 'S'
    {{0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}},
    // 0x54 'T'
    {{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    // 0x55 'U'
    {{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    // 0x56 'V'
    {{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    // 0x57 'W'
    {{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
    // 0x58 'X'
    {{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    // 0x59 'Y'
    {{0x11,0x11,0x11,0x0A,0x04,0x04,0x04}},
    // 0x5A 'Z'
    {{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    // 0x5B '['
    {{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}},
    // 0x5C '\\'
    {{0x00,0x10,0x08,0x04,0x02,0x01,0x00}},
    // 0x5D ']'
    {{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}},
    // 0x5E '^'
    {{0x04,0x0A,0x11,0x00,0x00,0x00,0x00}},
    // 0x5F '_'
    {{0x00,0x00,0x00,0x00,0x00,0x00,0x1F}},
    // 0x60 '`'
    {{0x08,0x04,0x02,0x00,0x00,0x00,0x00}},
    // 0x61 'a'
    {{0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}},
    // 0x62 'b'
    {{0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}},
    // 0x63 'c'
    {{0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}},
    // 0x64 'd'
    {{0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}},
    // 0x65 'e'
    {{0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}},
    // 0x66 'f'
    {{0x06,0x09,0x08,0x1C,0x08,0x08,0x08}},
    // 0x67 'g'
    {{0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}},
    // 0x68 'h'
    {{0x10,0x10,0x1E,0x11,0x11,0x11,0x11}},
    // 0x69 'i'
    {{0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}},
    // 0x6A 'j'
    {{0x02,0x00,0x06,0x02,0x02,0x12,0x0C}},
    // 0x6B 'k'
    {{0x10,0x10,0x12,0x14,0x18,0x14,0x12}},
    // 0x6C 'l'
    {{0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}},
    // 0x6D 'm'
    {{0x00,0x00,0x1A,0x15,0x15,0x11,0x11}},
    // 0x6E 'n'
    {{0x00,0x00,0x1E,0x11,0x11,0x11,0x11}},
    // 0x6F 'o'
    {{0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}},
    // 0x70 'p'
    {{0x00,0x00,0x1E,0x11,0x11,0x1E,0x10}},
    // 0x71 'q'
    {{0x00,0x00,0x0F,0x11,0x11,0x0F,0x01}},
    // 0x72 'r'
    {{0x00,0x00,0x16,0x19,0x10,0x10,0x10}},
    // 0x73 's'
    {{0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E}},
    // 0x74 't'
    {{0x08,0x08,0x1C,0x08,0x08,0x09,0x06}},
    // 0x75 'u'
    {{0x00,0x00,0x11,0x11,0x11,0x11,0x0F}},
    // 0x76 'v'
    {{0x00,0x00,0x11,0x11,0x11,0x0A,0x04}},
    // 0x77 'w'
    {{0x00,0x00,0x11,0x11,0x15,0x15,0x0A}},
    // 0x78 'x'
    {{0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}},
    // 0x79 'y'
    {{0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}},
    // 0x7A 'z'
    {{0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}},
    // 0x7B '{'
    {{0x02,0x04,0x04,0x08,0x04,0x04,0x02}},
    // 0x7C '|'
    {{0x04,0x04,0x04,0x04,0x04,0x04,0x04}},
    // 0x7D '}'
    {{0x08,0x04,0x04,0x02,0x04,0x04,0x08}},
    // 0x7E '~'
    {{0x00,0x08,0x15,0x02,0x00,0x00,0x00}},
    // 0x7F DEL -> blank
    {{0,0,0,0,0,0,0}},
};

void put_pixel(RGBAImage& img, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= img.width || y < 0 || y >= img.height) return;
    uint8_t* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

void fill_rect(RGBAImage& img, int x0, int y0, int x1, int y1,
               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) put_pixel(img, x, y, r, g, b, a);
    }
}

void draw_line(RGBAImage& img, int x0, int y0, int x1, int y1,
               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    // Bresenham
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int x = x0, y = y0;
    while (true) {
        put_pixel(img, x, y, r, g, b, a);
        if (x == x1 && y == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

void draw_char(RGBAImage& img, int x0, int y0, char c,
               uint8_t r, uint8_t g, uint8_t b) {
    if (static_cast<unsigned char>(c) >= 128) c = '?';
    const Glyph& g_ = kFont[static_cast<unsigned char>(c)];
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (g_.rows[row] & (1u << (4 - col))) {
                put_pixel(img, x0 + col, y0 + row, r, g, b, 255);
            }
        }
    }
}

int text_width(const std::string& s) {
    return static_cast<int>(s.size()) * 6 - 1;  // 5 px + 1 px spacing
}

void draw_text(RGBAImage& img, int x, int y, const std::string& s,
               uint8_t r, uint8_t g, uint8_t b) {
    int cx = x;
    for (char c : s) {
        draw_char(img, cx, y, c, r, g, b);
        cx += 6;
    }
}

std::string format_freq(float hz) {
    if (hz >= 1000.0f) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1fk", hz / 1000.0f);
        return std::string(buf);
    } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(hz));
        return std::string(buf);
    }
}

std::string format_db(float db) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0fdB", static_cast<double>(db));
    return std::string(buf);
}

std::string format_amp(float a) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(a));
    return std::string(buf);
}

} // namespace

// ============================================================================
// Static helpers
// ============================================================================

std::vector<float> SpectrumRenderer::aggregate(const SpectralDataset& dataset,
                                                SpectrumAggregation mode) {
    const int nb = dataset.num_frequency_bins();
    std::vector<float> out(nb, 0.0f);
    if (nb <= 0 || dataset.frame_count() <= 0) return out;

    if (mode == SpectrumAggregation::FirstFrame) {
        for (int k = 0; k < nb; ++k) out[k] = dataset.frame(0).magnitudes[k];
        return out;
    }
    if (mode == SpectrumAggregation::Max || mode == SpectrumAggregation::Peak) {
        for (int k = 0; k < nb; ++k) {
            float m = -std::numeric_limits<float>::infinity();
            for (int i = 0; i < dataset.frame_count(); ++i) {
                m = std::max(m, dataset.frame(i).magnitudes[k]);
            }
            out[k] = std::isfinite(m) ? m : 0.0f;
        }
        return out;
    }
    // Mean
    for (int i = 0; i < dataset.frame_count(); ++i) {
        const auto& f = dataset.frame(i);
        for (int k = 0; k < nb; ++k) {
            const float v = f.magnitudes[k];
            if (std::isfinite(v)) out[k] += v;
        }
    }
    const float inv = 1.0f / static_cast<float>(dataset.frame_count());
    for (auto& v : out) v *= inv;
    return out;
}

float SpectrumRenderer::freq_to_col(float freq_hz) const {
    const float fmin = std::max(1.0f, cfg_.freq_min_hz);
    const float fmax = cfg_.freq_max_hz;
    if (fmax <= fmin) return 0.0f;
    float t = hz_to_unit(freq_hz, static_cast<int>(cfg_.freq_scale),
                          fmin, fmax, cfg_.cqt_center_hz, cfg_.cqt_q);
    return t * static_cast<float>(cfg_.width - 1);
}

int SpectrumRenderer::bin_to_col(int bin, int num_bins, int sample_rate,
                                  float resolved_fmax_hz) const {
    if (num_bins <= 0 || sample_rate <= 0) return 0;
    const float freq = static_cast<float>(bin) * static_cast<float>(sample_rate)
                       / static_cast<float>(2 * (num_bins - 1));
    // Use the resolved fmax (not the raw config) so auto-resolution works.
    const float fmin = std::max(1.0f, cfg_.freq_min_hz);
    const float fmax = (resolved_fmax_hz > fmin) ? resolved_fmax_hz : fmin;
    float t;
    if (cfg_.freq_scale == FrequencyScale::Logarithmic) {
        const float lfmin = std::log10(fmin);
        const float lfmax = std::log10(fmax);
        const float lf = std::log10(std::max(freq, 1.0f));
        t = (lf - lfmin) / (lfmax - lfmin);
    } else {
        t = (freq - fmin) / (fmax - fmin);
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return static_cast<int>(std::lround(t * static_cast<float>(cfg_.width - 1)));
}

int SpectrumRenderer::y_to_row(float y) const {
    if (y < 0.0f) y = 0.0f;
    if (y > 1.0f) y = 1.0f;
    const int h = cfg_.height;
    int row = static_cast<int>((1.0f - y) * static_cast<float>(h - 1));
    if (row < 0) row = 0;
    if (row > h - 1) row = h - 1;
    return row;
}

void SpectrumRenderer::color_for_value(float t, ColorMap cm,
                                      uint8_t& r, uint8_t& g, uint8_t& b) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    switch (cm) {
        case ColorMap::Heat:    SpectrogramRenderer::color_heat(t, r, g, b); break;
        case ColorMap::Viridis:
        default:                SpectrogramRenderer::color_viridis(t, r, g, b); break;
    }
}

int SpectrumRenderer::freq_to_x(float freq_hz, float fmin, float fmax,
                                 int width, FrequencyScale scale) {
    if (fmax <= fmin || width <= 0) return 0;
    float t = hz_to_unit(freq_hz, static_cast<int>(scale), fmin, fmax);
    return static_cast<int>(std::lround(t * static_cast<float>(width - 1)));
}

// ============================================================================
// Render
// ============================================================================

SpectrumError SpectrumRenderer::render(const SpectralDataset& dataset,
                                       RGBAImage& out,
                                       const std::atomic<bool>* cancel) const {
    out.clear();
    // STFT-only renderer (uniform-bin line plot); refuse the misleading
    // alternative of drawing non-STFT bins as FFT data.
    if (!dataset.representation().is_stft()) return SpectrumError::UnsupportedRepresentation;
    if (cfg_.width <= 0 || cfg_.height <= 0) return SpectrumError::InvalidDimensions;
    if (dataset.frame_count() <= 0 || dataset.num_frequency_bins() <= 0) {
        return SpectrumError::EmptyDataset;
    }

    const float fmin = std::max(1.0f, cfg_.freq_min_hz);
    const float fmax = (cfg_.freq_max_hz > 0.0f) ? cfg_.freq_max_hz
                                                : dataset.nyquist_frequency();
    if (fmax <= fmin) return SpectrumError::InvalidFrequencyRange;

    const int W = cfg_.width;
    const int H = cfg_.height;
    out.width = W;
    out.height = H;
    out.pixels.assign(static_cast<size_t>(W) * H * 4, 0);

    // Background
    for (int y = 0; y < H; ++y) {
        if (cancel && cancel->load(std::memory_order_acquire)) {
            out.clear();
            return SpectrumError::Cancelled;
        }
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &out.pixels[(static_cast<size_t>(y) * W + x) * 4];
            p[0] = cfg_.bg_r; p[1] = cfg_.bg_g; p[2] = cfg_.bg_b; p[3] = cfg_.bg_a;
        }
    }

    // Reserve top/bottom strips for labels
    const int label_strip = cfg_.draw_labels ? 8 : 0;
    const int plot_y0 = label_strip;
    const int plot_y1 = H - 1;
    const int plot_h = plot_y1 - plot_y0 + 1;

    // Unit domain: map fmin/fmax → [0,1] for grid/label spacing
    const float unit_min = hz_to_unit(fmin, static_cast<int>(cfg_.freq_scale), fmin, fmax, cfg_.cqt_center_hz, cfg_.cqt_q);
    const float unit_max = hz_to_unit(fmax, static_cast<int>(cfg_.freq_scale), fmin, fmax, cfg_.cqt_center_hz, cfg_.cqt_q);

    // ---- Grid ----
    if (cfg_.draw_grid) {
        // Vertical (X) grid — evenly spaced in unit domain, mapped to x
        const int nx = std::max(1, cfg_.grid_divisions_x);
        for (int i = 1; i < nx; ++i) {
            const float frac = static_cast<float>(i) / static_cast<float>(nx);
            const float unit = unit_min + frac * (unit_max - unit_min);
            const float f = unit_to_hz(unit, static_cast<int>(cfg_.freq_scale), cfg_.cqt_center_hz, cfg_.cqt_q);
            const int x = freq_to_x(f, fmin, fmax, W, cfg_.freq_scale);
            for (int y = plot_y0; y <= plot_y1; ++y) {
                put_pixel(out, x, y, cfg_.grid_r, cfg_.grid_g, cfg_.grid_b, 255);
            }
        }
        // Horizontal (Y) grid
        const int ny = std::max(1, cfg_.grid_divisions_y);
        for (int i = 1; i < ny; ++i) {
            const float frac = static_cast<float>(i) / static_cast<float>(ny);
            const int y = plot_y0 + static_cast<int>(frac * plot_h);
            for (int x = 0; x < W; ++x) {
                put_pixel(out, x, y, cfg_.grid_r, cfg_.grid_g, cfg_.grid_b, 255);
            }
        }
    }

    // ---- Aggregate spectrum ----
    std::vector<float> spec = aggregate(dataset, cfg_.aggregation);
    if (spec.empty()) return SpectrumError::EmptyDataset;

    // Map bin index -> x, mag -> y
    // Pre-compute (x, y) per bin, then connect with lines.
    const int nb = dataset.num_frequency_bins();
    const int sr = dataset.sample_rate();
    const float ref = std::max(1e-12f, cfg_.reference_amplitude);

    std::vector<int> xs(nb, 0);
    std::vector<int> ys(nb, 0);
    for (int k = 0; k < nb; ++k) {
        int xb = bin_to_col(k, nb, sr, fmax);
        if (xb < 0) xb = 0;
        if (xb > W - 1) xb = W - 1;
        xs[k] = xb;
        const float mag = spec[k];
        float y_norm;
        if (cfg_.y_scale == SpectrumScale::Decibels) {
            if (!std::isfinite(mag) || mag <= 0.0f) {
                y_norm = 0.0f;
            } else {
                const float db = 20.0f * std::log10(mag / ref);
                float t = (cfg_.db_ceiling > cfg_.db_floor)
                    ? (db - cfg_.db_floor) / (cfg_.db_ceiling - cfg_.db_floor)
                    : 0.0f;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                y_norm = t;
            }
        } else {
            float t = (cfg_.y_max_amplitude > 0.0f)
                ? (mag / cfg_.y_max_amplitude)
                : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            y_norm = t;
        }
        ys[k] = y_to_row(y_norm);
    }

    // Draw polyline with thickness
    for (int k = 1; k < nb; ++k) {
        if ((k & 255) == 0 && cancel && cancel->load(std::memory_order_acquire)) {
            out.clear();
            return SpectrumError::Cancelled;
        }
        const int t = std::max(1, cfg_.line_thickness);
        for (int dx = 0; dx < t; ++dx) {
            // Color: gradient by y (top of plot -> high values)
            const float t_color = static_cast<float>(H - 1 - ys[k]) / static_cast<float>(H - 1);
            uint8_t cr, cg, cb;
            color_for_value(t_color, cfg_.color_map, cr, cg, cb);
            for (int dy = -(t/2); dy <= t/2; ++dy) {
                draw_line(out, xs[k-1] + dx, ys[k-1] + dy, xs[k] + dx, ys[k] + dy,
                          cr, cg, cb, 255);
            }
        }
    }

    // ---- Labels ----
    if (cfg_.draw_labels && label_strip > 0) {
        // Y-axis labels (top of image)
        const int ny = std::max(1, cfg_.grid_divisions_y);
        for (int i = 0; i <= ny; ++i) {
            const float frac = static_cast<float>(i) / static_cast<float>(ny);
            const int y = plot_y0 + static_cast<int>(frac * plot_h);
            std::string s;
            if (cfg_.y_scale == SpectrumScale::Decibels) {
                const float db = cfg_.db_ceiling - frac * (cfg_.db_ceiling - cfg_.db_floor);
                s = format_db(db);
            } else {
                const float a = cfg_.y_max_amplitude - frac * cfg_.y_max_amplitude;
                s = format_amp(a);
            }
            draw_text(out, 2, y - 3, s, cfg_.label_r, cfg_.label_g, cfg_.label_b);
        }
        // X-axis labels (bottom strip) — evenly spaced in unit domain
        const int nx = std::max(1, cfg_.grid_divisions_x);
        for (int i = 0; i <= nx; ++i) {
            float frac = static_cast<float>(i) / static_cast<float>(nx);
            const float unit = unit_min + frac * (unit_max - unit_min);
            const float f = unit_to_hz(unit, static_cast<int>(cfg_.freq_scale), cfg_.cqt_center_hz, cfg_.cqt_q);
            const std::string s = format_freq(f);
            const int x = freq_to_x(f, fmin, fmax, W, cfg_.freq_scale);
            int tx = x - text_width(s) / 2;
            if (tx < 0) tx = 0;
            if (tx + text_width(s) >= W) tx = W - text_width(s) - 1;
            draw_text(out, tx, H - 7, s, cfg_.label_r, cfg_.label_g, cfg_.label_b);
        }
    }

    return SpectrumError::Ok;
}

SpectrumError SpectrumRenderer::render_to_png(const SpectralDataset& dataset,
                                              const std::string& png_path,
                                              const std::atomic<bool>* cancel) const {
    RGBAImage img;
    SpectrumError err = render(dataset, img, cancel);
    if (err != SpectrumError::Ok) return err;
    if (!PNGEncoder::write_rgba(png_path, img.width, img.height, img.pixels.data())) {
        return SpectrumError::InvalidDimensions;
    }
    return SpectrumError::Ok;
}

} // namespace Spectral
