#pragma once
#include "spectral_dataset.h"
#include <vector>
#include <functional>

namespace Spectral {

struct BandConfig {
    float freq_low;    // Hz
    float freq_high;   // Hz
    int n_fft;         // FFT size for this band
};

class MultiBandAnalyzer {
public:
    struct AnalyzeResult {
        SpectralDataset dataset;
        float compute_ms;
    };

    static std::vector<BandConfig> default_bands(float nyquist_hz);

    static AnalyzeResult analyze(
        const std::vector<float>& samples,
        int sample_rate,
        int n_fft = 1024,
        int hop_size = 256
    );

    static AnalyzeResult analyze_single(
        const std::vector<float>& samples,
        int sample_rate,
        int n_fft,
        int hop_size
    );
};

} // namespace Spectral
