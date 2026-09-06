#pragma once

#include "spectral_dataset.h"
#include <vector>
#include <string>

struct RenderOptions {
    int width{800};
    int height{600};
    float freq_min{20.0f};
    float freq_max{20000.0f};
    float db_floor{-120.0f};
    float db_ceiling{0.0f};
    bool log_scale{true};
};

class SpectralRenderer {
public:
    virtual ~SpectralRenderer() = default;

    // Initialize renderer with dataset and options
    virtual bool init(const SpectralDataset& dataset, const RenderOptions& opts) = 0;

    // Render one frame to pixel data
    // Returns false if frame index out of range
    virtual bool render_frame(int frame_index, std::vector<unsigned char>& out_pixels) = 0;

    // Render all frames
    virtual bool render_all(std::vector<std::vector<unsigned char>>& out_frames) = 0;

    // Get output dimensions
    int width() const { return output_width_; }
    int height() const { return output_height_; }

    // Get frequency range
    float freq_min() const { return opts_.freq_min; }
    float freq_max() const { return opts_.freq_max; }

protected:
    RenderOptions opts_;
    int output_width_{0};
    int output_height_{0};
};