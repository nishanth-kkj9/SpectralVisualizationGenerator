#pragma once

// Phase 10 — Video renderer.
// Slices a SpectralDataset into time windows, renders each as a frame,
// and pipes them to a VideoEncoder.

#include "spectral_dataset.h"
#include "spectrogram_renderer.h"
#include "video_encoder.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace Spectral {

struct VideoRendererConfig {
    int width = 1024;
    int height = 512;
    int fps = 30;
    std::string codec = "libx264";
    int crf = 18;
    std::string color_map_name = "viridis"; // "viridis" or "heat"
    FrequencyScale freq_scale = FrequencyScale::Logarithmic;
    float freq_min_hz = 20.0f;
    float freq_max_hz = 0.0f;  // 0 = auto (nyquist)
    float db_floor = -80.0f;
    float db_ceiling = 0.0f;
    float window_seconds = 5.0f;  // time window visible per frame (seconds)
};

enum class VideoRenderError {
    Ok = 0,
    EmptyDataset,
    EncoderOpenFailed,
    EncoderWriteFailed,
    EncoderCloseFailed,
    Cancelled,  // caller-requested cancellation; encoder terminated
    UnsupportedRepresentation,  // only STFT datasets can be rendered
};

class VideoRenderer {
public:
    VideoRenderer() = default;
    explicit VideoRenderer(const VideoRendererConfig& cfg) : cfg_(cfg) {}

    // Render entire dataset to video file. cancel (may be null) is
    // observed between frames; on Cancelled the encoder child is
    // terminated (never left running) and the partial file is untouched
    // by this layer (the pipeline removes its temp).
    VideoRenderError render(const SpectralDataset& dataset,
                            const std::string& output_path,
                            const std::atomic<bool>* cancel = nullptr) const;

    // Render a single frame at the given time. Returns RGBA pixels.
    // Useful for testing without video encoding.
    VideoRenderError render_frame(const SpectralDataset& dataset,
                                  double time_sec,
                                  RGBAImage& out) const;

    const VideoRendererConfig& config() const { return cfg_; }
    void set_config(const VideoRendererConfig& c) { cfg_ = c; }

private:
    VideoRendererConfig cfg_;
};

} // namespace Spectral
