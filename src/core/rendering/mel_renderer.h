#pragma once

// Phase 9 — Mel spectrogram image renderer (CPU).
//
// Representation-aware counterpart to SpectrogramRenderer: it maps the
// dataset's EXPLICIT Mel band centers to pixels instead of assuming FFT
// bins. Each image row corresponds to Mel bands (not k*sr/N); time maps
// to frames exactly like the STFT path. STFT datasets are refused here
// (use SpectrogramRenderer); non-Mel datasets are refused everywhere.
//
// Decibels use the same convention as the STFT renderer: magnitudes are
// amplitude-domain, mapped with 20*log10(mag/reference), normalized to
// [db_floor, db_ceiling]. Shared helpers (color_map, normalize_db) come
// from SpectrogramRenderer.

#include "spectral_dataset.h"
#include "spectrogram_renderer.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Spectral {

struct MelSpectrogramConfig {
    int width = 1024;
    int height = 512;
    ColorMap color_map = ColorMap::Viridis;
    float db_floor = -90.0f;
    float db_ceiling = 0.0f;
    float reference_amplitude = 1.0f;
};

enum class MelRenderError {
    Ok = 0,
    EmptyDataset,
    InvalidDimensions,
    UnsupportedRepresentation,  // only Mel datasets can be rendered here
    Cancelled,                  // caller-requested cancellation
};

class MelSpectrogramRenderer {
public:
    MelSpectrogramRenderer() = default;
    explicit MelSpectrogramRenderer(const MelSpectrogramConfig& cfg) : cfg_(cfg) {}

    // Render synchronously. The output image is filled only on Ok.
    // cancel (may be null) is observed per row.
    MelRenderError render(const SpectralDataset& dataset, RGBAImage& out,
                          const std::atomic<bool>* cancel = nullptr) const;

    // Convenience: render + write PNG.
    MelRenderError render_to_png(const SpectralDataset& dataset,
                                 const std::string& png_path,
                                 const std::atomic<bool>* cancel = nullptr) const;

    const MelSpectrogramConfig& config() const { return cfg_; }
    void set_config(const MelSpectrogramConfig& c) { cfg_ = c; }

private:
    MelSpectrogramConfig cfg_;
};

} // namespace Spectral
