// Phase 9 — Mel spectrogram image renderer implementation.

#include "mel_renderer.h"

#include "png_encoder.h"

#include <algorithm>
#include <cmath>

namespace Spectral {

namespace {

// dB mapping shared semantically with the STFT renderer: amplitude-domain
// magnitudes via 20*log10, floored (never NaN out).
float mel_mag_to_db(float mag, float db_floor, float reference) {
    if (!(mag > 0.0f) || !(reference > 0.0f)) return db_floor;
    return 20.0f * std::log10(mag / reference);
}

} // namespace

MelRenderError MelSpectrogramRenderer::render(const SpectralDataset& dataset,
                                              RGBAImage& out,
                                              const std::atomic<bool>* cancel) const {
    out.clear();
    // Representation contract: Mel bands only. STFT data belongs to
    // SpectrogramRenderer; anything else has no mapping here.
    if (dataset.representation().kind != RepresentationKind::Mel)
        return MelRenderError::UnsupportedRepresentation;
    if (cfg_.width <= 0 || cfg_.height <= 0) return MelRenderError::InvalidDimensions;
    if (dataset.frame_count() <= 0 || dataset.num_frequency_bins() <= 0)
        return MelRenderError::EmptyDataset;

    const int W = cfg_.width;
    const int H = cfg_.height;
    const int Nf = dataset.frame_count();
    const int Nb = dataset.num_frequency_bins();
    out.width = W;
    out.height = H;
    out.pixels.assign(static_cast<size_t>(W) * H * 4, 0);

    const double total_dur = dataset.total_duration();
    const float ref =
        dataset.normalization_info().reference_amplitude > 0.0f
            ? dataset.normalization_info().reference_amplitude
            : cfg_.reference_amplitude;

    // Precompute the source frame per column (nearest in time) and the
    // source band per row (band 0 at the bottom, like frequency).
    std::vector<int> col_frame(W, 0);
    for (int x = 0; x < W; ++x) {
        const double t = (total_dur > 0.0) ? (static_cast<double>(x) / W) * total_dur : 0.0;
        int fi = 0;
        if (dataset.frame_count() > 1 && total_dur > 0.0) {
            fi = static_cast<int>(t / total_dur * (Nf - 1) + 0.5);
            if (fi < 0) fi = 0;
            if (fi >= Nf) fi = Nf - 1;
        }
        col_frame[x] = fi;
    }
    for (int y = 0; y < H; ++y) {
        if (cancel && cancel->load(std::memory_order_acquire)) {
            out.clear();
            return MelRenderError::Cancelled;
        }
        // Row 0 (top) = highest band; row H-1 (bottom) = band 0.
        // H == 1 has no pair of extremes to interpolate between, so it maps
        // to one deterministic band (the middle of the representation). H > 1
        // keeps the historical stretched mapping exactly.
        int band = 0;
        if (Nb == 1) {
            band = 0;
        } else if (H == 1) {
            band = (Nb - 1) / 2;
        } else {
            band = static_cast<int>((1.0 - static_cast<double>(y) / (H - 1)) *
                                        (Nb - 1) +
                                    0.5);
        }
        if (band < 0) band = 0;
        if (band >= Nb) band = Nb - 1;
        for (int x = 0; x < W; ++x) {
            const auto& fr = dataset.frame(col_frame[x]);
            const float mag =
                (band < static_cast<int>(fr.magnitudes.size())) ? fr.magnitudes[band] : 0.0f;
            const float db = mel_mag_to_db(mag, cfg_.db_floor, ref);
            const float t = SpectrogramRenderer::normalize_db(db, cfg_.db_floor,
                                                              cfg_.db_ceiling);
            uint8_t r, g, b;
            SpectrogramRenderer::color_map(cfg_.color_map, t, r, g, b);
            uint8_t* p = &out.pixels[(static_cast<size_t>(y) * W + x) * 4];
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = 255;
        }
    }
    return MelRenderError::Ok;
}

MelRenderError MelSpectrogramRenderer::render_to_png(const SpectralDataset& dataset,
                                                     const std::string& png_path,
                                                     const std::atomic<bool>* cancel) const {
    RGBAImage img;
    MelRenderError err = render(dataset, img, cancel);
    if (err != MelRenderError::Ok) return err;
    if (!PNGEncoder::write_rgba(png_path, img.width, img.height, img.pixels.data())) {
        return MelRenderError::InvalidDimensions;
    }
    return MelRenderError::Ok;
}

} // namespace Spectral
