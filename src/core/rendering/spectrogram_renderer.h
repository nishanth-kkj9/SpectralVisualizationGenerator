#pragma once

// Phase 6 — Spectrogram image renderer.
// Converts a SpectralDataset to a static RGBA image (PNG).
// Does NOT recompute FFT/STFT. Renderer reads dataset only.

#include "spectral_dataset.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Spectral {

// ============================================================================
// RGBA image
// ============================================================================
struct RGBAImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;  // row-major, RGBA8, length = width*height*4

    bool valid() const { return width > 0 && height > 0 && pixels.size() == static_cast<size_t>(width) * height * 4; }
    void clear() { width = height = 0; pixels.clear(); }
};

// ============================================================================
// Color map identifiers
// ============================================================================
enum class ColorMap {
    Viridis = 0,  // Perceptual, scientifically defensible default
    Heat = 1,     // Conventional black->red->yellow->white
};

// ============================================================================
// Frequency scale
// ============================================================================
enum class FrequencyScale {
    Linear = 0,
    Logarithmic = 1,
    Mel = 2,
    Bark = 3,
    Erb = 4,
    CQT = 5,
};

// ============================================================================
// Interpolation for sub-pixel frequency mapping
// ============================================================================
enum class Interpolation {
    Nearest = 0,    // Fastest; blocky
    Bilinear = 1,   // Smoother; default
};

// ============================================================================
// Render configuration
// ============================================================================
struct SpectrogramConfig {
    int width = 1024;                          // Output image width  (px)
    int height = 512;                          // Output image height (px)
    ColorMap color_map = ColorMap::Viridis;    // Default: perceptual
    FrequencyScale freq_scale = FrequencyScale::Logarithmic;
    Interpolation interpolation = Interpolation::Bilinear;
    float db_floor = -90.0f;                   // Min dB (mapped to colormap start)
    float db_ceiling = 0.0f;                   // Max dB (mapped to colormap end)
    float freq_min_hz = 20.0f;                 // Lower bound (clamped > 0 for log)
    float freq_max_hz = 0.0f;                  // 0 = auto = nyquist
    float cqt_center_hz = 440.0f;              // CQT center frequency
    float cqt_q = 12.0f;                       // CQT bins per octave
    uint32_t seed = 0;                         // Reserved; not used in current pixel path
    // Background color used for out-of-range / silent samples
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0, bg_a = 0;
};

// ============================================================================
// Errors
// ============================================================================
enum class RenderError {
    Ok = 0,
    EmptyDataset,
    InvalidDimensions,
    InvalidFrequencyRange,
    Cancelled,  // caller-requested cancellation; out is cleared
};

// ============================================================================
// Spectrogram renderer
// ============================================================================
class SpectrogramRenderer {
public:
    SpectrogramRenderer() = default;
    explicit SpectrogramRenderer(const SpectrogramConfig& cfg) : cfg_(cfg) {}

    // Render synchronously. Returns Ok on success.
    // The output image is filled only on Ok. On any error, out is cleared.
    // cancel (may be null) is observed at row/frame boundaries; on
    // Cancelled the threads finish their current row and out is cleared.
    RenderError render(const SpectralDataset& dataset, RGBAImage& out,
                       const std::atomic<bool>* cancel = nullptr) const;

    // Convenience: render + write PNG. Equivalent to render() then write_png().
    RenderError render_to_png(const SpectralDataset& dataset,
                              const std::string& png_path,
                              const std::atomic<bool>* cancel = nullptr) const;

    // Phase 17 — GPU path via D3D11 compute shaders. Falls back to CPU render()
    // when GPU is unavailable. Output layout identical to render().
    // cancel is checked before dispatch; an in-flight GPU dispatch itself
    // is not preemptible (bounded by one dispatch), then CPU path checks apply.
    RenderError render_gpu(const SpectralDataset& dataset, RGBAImage& out,
                           const std::atomic<bool>* cancel = nullptr) const;

    // Accessors
    const SpectrogramConfig& config() const { return cfg_; }
    void set_config(const SpectrogramConfig& c) { cfg_ = c; }

    // ---- Color map helpers (public for tests) ----
    // t in [0,1] -> RGBA8. Out-of-range t is clamped.
    static void color_viridis(float t, uint8_t& r, uint8_t& g, uint8_t& b);
    static void color_heat(float t, uint8_t& r, uint8_t& g, uint8_t& b);
    static void color_map(ColorMap cm, float t, uint8_t& r, uint8_t& g, uint8_t& b);

    // dB normalization helpers (public for tests)
    static float normalize_db(float db, float db_floor, float db_ceiling);

private:
    SpectrogramConfig cfg_;
};

} // namespace Spectral
