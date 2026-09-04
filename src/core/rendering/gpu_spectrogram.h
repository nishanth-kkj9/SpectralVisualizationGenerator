#pragma once

// Phase 17 — GPU spectrogram renderer via D3D11 compute shaders.
// Replicates CPU SpectrogramRenderer::render() on GPU.
// Falls back to CPU if D3D11 init fails.

#include "spectrogram_renderer.h"
#include "d3d11_context.h"
#include <memory>
#include <vector>

namespace Spectral {

class GpuSpectrogram {
public:
    explicit GpuSpectrogram(D3D11Context& ctx);
    ~GpuSpectrogram();

    GpuSpectrogram(const GpuSpectrogram&) = delete;
    GpuSpectrogram& operator=(const GpuSpectrogram&) = delete;

    bool is_available() const;
    RenderError render(const SpectralDataset& dataset,
                       const SpectrogramConfig& cfg, RGBAImage& out);

    static void build_viridis_lut(std::vector<float>& lut);

    ID3D11ComputeShader* spectrogram_shader() const { return spectrogram_shader_.Get(); }
    ID3D11ComputeShader* colormap_shader() const { return colormap_shader_.Get(); }

private:
    bool ensure_shaders();

    D3D11Context& ctx_;
    bool shaders_ready_ = false;

    // Cached shaders
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> spectrogram_shader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> colormap_shader_;
};

} // namespace Spectral
