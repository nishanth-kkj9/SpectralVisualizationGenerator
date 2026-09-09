#pragma once

// Phase 7 — Static frequency spectrum image renderer.
// Renders a single 1-D spectrum line plot from a SpectralDataset.
// Consumes existing spectral data; does not recompute FFT/STFT.

#include "spectral_dataset.h"
#include "spectrogram_renderer.h"  // for RGBAImage + ColorMap (shared types)

#include <cstdint>
#include <string>
#include <vector>

namespace Spectral {

// ============================================================================
// Y-axis scale
// ============================================================================
enum class SpectrumScale {
    Linear = 0,   // 0..max amplitude
    Decibels = 1, // dB over reference; clamps to [db_floor, db_ceiling]
};

// ============================================================================
// Aggregation mode (how multiple frames collapse to one spectrum)
// ============================================================================
enum class SpectrumAggregation {
    Mean = 0,     // average magnitude per bin
    Max = 1,      // max magnitude per bin
    Peak = 2,     // max hold (alias of Max for clarity)
    FirstFrame = 3, // use first frame only
};

// ============================================================================
// Render configuration
// ============================================================================
struct SpectrumConfig {
    int width = 1024;
    int height = 512;

    // Frequency axis
    FrequencyScale freq_scale = FrequencyScale::Logarithmic;
    float freq_min_hz = 20.0f;     // 0 = auto = dataset's bin 0 frequency
    float freq_max_hz = 0.0f;      // 0 = auto = nyquist
    float cqt_center_hz = 440.0f;
    float cqt_q = 12.0f;

    // Y axis
    SpectrumScale y_scale = SpectrumScale::Decibels;
    float db_floor = -90.0f;
    float db_ceiling = 0.0f;
    float y_max_amplitude = 1.0f;   // for linear scale (default 1.0)
    float reference_amplitude = 1.0f;

    // Aggregation
    SpectrumAggregation aggregation = SpectrumAggregation::Mean;

    // Plot styling
    ColorMap color_map = ColorMap::Viridis; // applied to line color (gradient by value)
    bool draw_grid = true;
    int grid_divisions_x = 8;   // in linear; in log: decades
    int grid_divisions_y = 6;
    bool draw_labels = true;

    // Background / text
    uint8_t bg_r = 0,  bg_g = 0,  bg_b = 0,  bg_a = 255;
    uint8_t fg_r = 240, fg_g = 240, fg_b = 240;
    uint8_t grid_r = 64, grid_g = 64, grid_b = 64;
    uint8_t label_r = 200, label_g = 200, label_b = 200;

    // Line
    int line_thickness = 2;
    uint32_t seed = 0; // reserved; deterministic path
};

// ============================================================================
// Errors
// ============================================================================
enum class SpectrumError {
    Ok = 0,
    EmptyDataset,
    InvalidDimensions,
    InvalidFrequencyRange,
    Cancelled,  // caller-requested cancellation; out is cleared
    UnsupportedRepresentation,  // only STFT datasets can be rendered
};

// ============================================================================
// Spectrum renderer
// ============================================================================
class SpectrumRenderer {
public:
    SpectrumRenderer() = default;
    explicit SpectrumRenderer(const SpectrumConfig& cfg) : cfg_(cfg) {}

    // Render synchronously. Returns Ok on success. On any error, out is cleared.
    // cancel (may be null) is observed at row boundaries.
    SpectrumError render(const SpectralDataset& dataset, RGBAImage& out,
                         const std::atomic<bool>* cancel = nullptr) const;

    // Convenience: render + write PNG.
    SpectrumError render_to_png(const SpectralDataset& dataset,
                                const std::string& png_path,
                                const std::atomic<bool>* cancel = nullptr) const;

    // Accessors
    const SpectrumConfig& config() const { return cfg_; }
    void set_config(const SpectrumConfig& c) { cfg_ = c; }

    // ---- Helpers (public for tests) ----
    // Compute the aggregated magnitude vector from a dataset.
    // Output length == dataset.num_frequency_bins().
    static std::vector<float> aggregate(const SpectralDataset& dataset,
                                        SpectrumAggregation mode);

    // Map frequency in Hz to column index in [0, width-1].
    float freq_to_col(float freq_hz) const;
    // Map normalized y in [0,1] (0=bottom=min, 1=top=max) to row index.
    int y_to_row(float y) const;
    // Map a bin index to a column. `resolved_fmax_hz` must be the effective
    // frequency maximum (i.e. auto-resolved against the dataset's nyquist
    // if cfg.freq_max_hz <= 0).
    int bin_to_col(int bin, int num_bins, int sample_rate,
                   float resolved_fmax_hz) const;

    // Color for a normalized y in [0,1] (mirrors spectrogram renderer).
    static void color_for_value(float t, ColorMap cm,
                                uint8_t& r, uint8_t& g, uint8_t& b);

    // Map frequency (Hz) to x pixel coordinate using the resolved fmin/fmax.
    // Both axes use cfg_.freq_scale; the result is in [0, cfg_.width-1].
    static int freq_to_x(float freq_hz, float fmin, float fmax,
                         int width, FrequencyScale scale);

private:
    SpectrumConfig cfg_;
};

} // namespace Spectral
