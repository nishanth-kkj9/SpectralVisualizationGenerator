// Phase 17 — GPU spectrogram rendering correctness.
// CPU reference vs render_gpu(): PSNR > 40dB, global SSIM > 0.99.
// Passes via CPU fallback when no GPU present.

#include "spectral_dataset.h"
#include "spectrogram_renderer.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace Spectral;

static SpectralDataset build_tone(int n_fft = 1024, int n_frames = 16,
                                  int sample_rate = 48000, float tone_hz = 440.0f) {
    SpectralDataset d;
    d.mutable_analysis_metadata().fft_size = n_fft;
    d.mutable_analysis_metadata().hop_size = n_fft / 4;
    d.mutable_analysis_metadata().sample_rate = sample_rate;
    d.mutable_analysis_metadata().nyquist_frequency = static_cast<float>(sample_rate) / 2.0f;
    d.mutable_analysis_metadata().num_frequency_bins = n_fft / 2 + 1;
    d.mutable_normalization_info().reference_amplitude = 1.0f;
    d.mutable_frequency_axis() = FrequencyAxis(n_fft, sample_rate);
    d.mutable_time_axis() = TimeAxis(n_frames, n_fft / 4, sample_rate);
    const int Nk = n_fft / 2 + 1;
    for (int f = 0; f < n_frames; ++f) {
        Spectral::SpectralFrame fr;
        fr.frame_index = f;
        fr.n_fft = n_fft;
        fr.timestamp = static_cast<double>(f * (n_fft / 4)) / sample_rate;
        fr.magnitudes.assign(Nk, 1e-6f);
        // tone peak + harmonic, decaying over time
        int k0 = static_cast<int>(tone_hz / (static_cast<float>(sample_rate) / n_fft));
        float decay = 1.0f / (1.0f + 0.1f * f);
        if (k0 > 0 && k0 < Nk) fr.magnitudes[k0] = decay;
        if (2 * k0 < Nk) fr.magnitudes[2 * k0] = 0.5f * decay;
        d.add_frame(fr);
    }
    return d;
}

static double psnr_db(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double mse = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        mse += d * d;
    }
    mse /= static_cast<double>(a.size());
    if (mse <= 0.0) return 100.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

// ponytail: global (single-window) SSIM over luma; enough for identical-output check
static double global_ssim(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const size_t n = a.size() / 4;
    double ma = 0.0, mb = 0.0;
    for (size_t i = 0; i < n; ++i) {
        ma += 0.299 * a[i * 4] + 0.587 * a[i * 4 + 1] + 0.114 * a[i * 4 + 2];
        mb += 0.299 * b[i * 4] + 0.587 * b[i * 4 + 1] + 0.114 * b[i * 4 + 2];
    }
    ma /= n;
    mb /= n;
    double va = 0.0, vb = 0.0, cov = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double la = 0.299 * a[i * 4] + 0.587 * a[i * 4 + 1] + 0.114 * a[i * 4 + 2] - ma;
        double lb = 0.299 * b[i * 4] + 0.587 * b[i * 4 + 1] + 0.114 * b[i * 4 + 2] - mb;
        va += la * la;
        vb += lb * lb;
        cov += la * lb;
    }
    va /= n;
    vb /= n;
    cov /= n;
    const double c1 = 6.5025, c2 = 58.5225;  // (0.01*255)^2, (0.03*255)^2
    double num = (2 * ma * mb + c1) * (2 * cov + c2);
    double den = (ma * ma + mb * mb + c1) * (va + vb + c2);
    return den > 0.0 ? num / den : 1.0;
}

static int check_combo(SpectrogramConfig cfg) {
    auto dataset = build_tone();
    SpectrogramRenderer cpu(cfg);
    RGBAImage ref, gpu;
    if (cpu.render(dataset, ref) != RenderError::Ok) {
        printf("FAIL: cpu render failed\n");
        return 1;
    }
    if (cpu.render_gpu(dataset, gpu) != RenderError::Ok) {
        printf("FAIL: gpu render failed\n");
        return 1;
    }
    if (ref.pixels.size() != gpu.pixels.size()) {
        printf("FAIL: size mismatch\n");
        return 1;
    }
    double psnr = psnr_db(ref.pixels, gpu.pixels);
    double ssim = global_ssim(ref.pixels, gpu.pixels);
    const char* cm = cfg.color_map == ColorMap::Heat ? "heat" : "viridis";
    const char* ip = cfg.interpolation == Interpolation::Nearest ? "nearest" : "bilinear";
    printf("  %s/%s: PSNR=%.1fdB SSIM=%.4f\n", cm, ip, psnr, ssim);
    if (psnr < 40.0) {
        printf("FAIL: PSNR below 40dB\n");
        return 1;
    }
    if (ssim < 0.99) {
        printf("FAIL: SSIM below 0.99\n");
        return 1;
    }
    return 0;
}

int main() {
    int fails = 0;
    for (int cm = 0; cm < 2; ++cm) {
        for (int ip = 0; ip < 2; ++ip) {
            SpectrogramConfig cfg;
            cfg.width = 256;
            cfg.height = 128;
            cfg.color_map = static_cast<ColorMap>(cm);
            cfg.interpolation = static_cast<Interpolation>(ip);
            fails += check_combo(cfg);
        }
    }
    printf("\nPhase 17 GPU render: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails;
}
