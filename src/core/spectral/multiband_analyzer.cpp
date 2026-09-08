#include "multiband_analyzer.h"
#include "spectral_backend.h"
#include "fft.h"
#include "fft_plan.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace Spectral {

std::vector<BandConfig> MultiBandAnalyzer::default_bands(float nyquist_hz) {
    std::vector<BandConfig> bands;

    float low_split = std::min(2000.0f, nyquist_hz / 4.0f);
    float mid_split = std::min(8000.0f, nyquist_hz * 3.0f / 4.0f);

    if (low_split > 0 && nyquist_hz > low_split) {
        BandConfig low;
        low.freq_low = 0;
        low.freq_high = low_split;
        low.n_fft = 4096;
        bands.push_back(low);
    }

    if (mid_split > low_split && nyquist_hz > mid_split) {
        BandConfig mid;
        mid.freq_low = low_split;
        mid.freq_high = mid_split;
        mid.n_fft = 1024;
        bands.push_back(mid);
    }

    if (nyquist_hz > mid_split) {
        BandConfig high;
        high.freq_low = mid_split;
        high.freq_high = nyquist_hz;
        high.n_fft = 256;
        bands.push_back(high);
    }

    return bands;
}

MultiBandAnalyzer::AnalyzeResult MultiBandAnalyzer::analyze(
    const std::vector<float>& samples,
    int sample_rate,
    int n_fft,
    int hop_size,
    const SpectralBackend* backend)
{
    auto t0 = std::chrono::high_resolution_clock::now();

    float nyquist = static_cast<float>(sample_rate) / 2.0f;
    auto bands = default_bands(nyquist);

    if (bands.empty()) {
        BandConfig single;
        single.freq_low = 0;
        single.freq_high = nyquist;
        single.n_fft = n_fft;
        bands.push_back(single);
    }

    int max_nfft = 0;
    for (const auto& b : bands) {
        max_nfft = std::max(max_nfft, b.n_fft);
    }

    int common_hop = max_nfft / 4;
    if (common_hop < 1) common_hop = 1;

    int num_frames = static_cast<int>((static_cast<int>(samples.size()) - max_nfft) / common_hop) + 1;
    if (num_frames < 1) num_frames = 1;

    int total_bins = max_nfft / 2 + 1;
    float bin_hz_max = static_cast<float>(sample_rate) / static_cast<float>(max_nfft);

    struct BandResult {
        std::vector<std::vector<float>> magnitudes;
        std::vector<std::vector<float>> power;
        int band_start_bin;
        int band_end_bin;
    };

    // ponytail: precompute windows per unique n_fft — eliminates per-frame alloc.
    std::vector<std::vector<float>> cached_windows;
    auto get_window = [&](int n_fft) -> const std::vector<float>& {
        for (const auto& w : cached_windows) {
            if (static_cast<int>(w.size()) == n_fft) return w;
        }
        cached_windows.push_back(window_hann(n_fft));
        return cached_windows.back();
    };

    std::vector<BandResult> band_results;

    for (const auto& band : bands) {
        int band_hop = band.n_fft / 4;
        if (band_hop < 1) band_hop = 1;

        int band_frames = static_cast<int>((static_cast<int>(samples.size()) - band.n_fft) / band_hop) + 1;
        if (band_frames < 1) band_frames = 1;

        int band_bins = band.n_fft / 2 + 1;

        int start_bin = static_cast<int>(std::round(band.freq_low / bin_hz_max));
        int end_bin = static_cast<int>(std::round(band.freq_high / bin_hz_max));
        if (start_bin < 0) start_bin = 0;
        if (end_bin >= total_bins) end_bin = total_bins - 1;

        BandResult br;
        br.band_start_bin = start_bin;
        br.band_end_bin = end_bin;
        br.magnitudes.resize(num_frames, std::vector<float>(total_bins, 0.0f));
        br.power.resize(num_frames, std::vector<float>(total_bins, 0.0f));

        float bin_hz_band = static_cast<float>(sample_rate) / static_cast<float>(band.n_fft);

        // Reusable plan + scratch for this band size (owned by this call,
        // never global). The backend path keeps its own execution model.
        FFTPlan band_plan;
        FFTWorkspace band_ws;
        const bool use_plan = (backend == nullptr);
        if (use_plan) {
            band_plan = FFTPlan::create(band.n_fft);
            band_ws.assign(band_plan);
        }

        for (int f = 0; f < num_frames; ++f) {
            int band_frame = static_cast<int>(static_cast<float>(f) * static_cast<float>(band_hop) / static_cast<float>(common_hop));
            if (band_frame >= band_frames) band_frame = band_frames - 1;
            if (band_frame < 0) band_frame = 0;

            int offset = band_frame * band_hop;
            if (offset + band.n_fft > static_cast<int>(samples.size())) break;

            const auto& win = get_window(band.n_fft);
            // Planned path fills only the reusable scratch buffer (no
            // per-frame vector). Other paths keep the legacy local buf.
            const bool planned =
                use_plan && band_plan.valid() && band_ws.valid();
            std::vector<complex_f> buf(planned ? 0 : band.n_fft);
            std::vector<complex_f>& work = planned ? band_ws.buf(0) : buf;
            for (int i = 0; i < band.n_fft; ++i) {
                work[i] = complex_f(samples[offset + i] * win[i], 0.0f);
            }
            if (backend) {
                backend->fft(buf);
            } else if (planned) {
                // Bit-identical to fft(buf); fail-safe fallback below.
                if (!fft_forward(band_plan, band_ws, 0)) {
                    buf.assign(band.n_fft, complex_f());
                    for (int i = 0; i < band.n_fft; ++i) {
                        buf[i] = complex_f(samples[offset + i] * win[i], 0.0f);
                    }
                    fft(buf);
                    auto [mag0, pwr0] = fft_magnitude_power(buf);
                    for (int b_bin = 0; b_bin < band_bins; ++b_bin) {
                        float freq = static_cast<float>(b_bin) * bin_hz_band;
                        int max_bin = static_cast<int>(std::round(freq / bin_hz_max));
                        if (max_bin >= start_bin && max_bin <= end_bin &&
                            max_bin < total_bins) {
                            br.magnitudes[f][max_bin] = mag0[b_bin];
                            br.power[f][max_bin] = pwr0[b_bin];
                        }
                    }
                    continue;
                }
                auto [mag, pwr] = fft_magnitude_power(band_ws.buf(0));
                for (int b_bin = 0; b_bin < band_bins; ++b_bin) {
                    float freq = static_cast<float>(b_bin) * bin_hz_band;
                    int max_bin = static_cast<int>(std::round(freq / bin_hz_max));
                    if (max_bin >= start_bin && max_bin <= end_bin &&
                        max_bin < total_bins) {
                        br.magnitudes[f][max_bin] = mag[b_bin];
                        br.power[f][max_bin] = pwr[b_bin];
                    }
                }
                continue;
            } else {
                fft(buf);
            }

            auto [mag, pwr] = backend ? backend->fft_magnitude_power(buf) : fft_magnitude_power(buf);

            for (int b_bin = 0; b_bin < band_bins; ++b_bin) {
                float freq = static_cast<float>(b_bin) * bin_hz_band;
                int max_bin = static_cast<int>(std::round(freq / bin_hz_max));
                if (max_bin >= start_bin && max_bin <= end_bin && max_bin < total_bins) {
                    br.magnitudes[f][max_bin] = mag[b_bin];
                    br.power[f][max_bin] = pwr[b_bin];
                }
            }
        }

        band_results.push_back(br);
    }

    // Build SpectralDataset using mutable accessors
    SpectralDataset dataset;

    auto& meta = dataset.mutable_analysis_metadata();
    meta.sample_rate = sample_rate;
    meta.fft_size = max_nfft;
    meta.hop_size = common_hop;
    meta.overlap_ratio = 0.75f;
    meta.nyquist_frequency = nyquist;
    meta.num_frequency_bins = total_bins;
    meta.window_type = "hann";
    meta.window_coherent_gain = window_coherent_gain(window_hann(max_nfft));
    meta.frame_duration_seconds = static_cast<double>(common_hop) / static_cast<double>(sample_rate);
    meta.total_duration_seconds = static_cast<double>(samples.size()) / static_cast<double>(sample_rate);
    meta.total_frames = num_frames;
    meta.analyzed_channels = 1;
    meta.analyzer_version = "2.0";
    meta.analysis_method = "multiband_stft";
    meta.band_count = static_cast<int>(bands.size());

    auto& freq_axis = dataset.mutable_frequency_axis();
    freq_axis.num_bins = total_bins;
    freq_axis.fft_size = max_nfft;
    freq_axis.sample_rate = sample_rate;
    freq_axis.nyquist = nyquist;
    freq_axis.resolution = bin_hz_max;
    freq_axis.bin_frequencies.resize(total_bins);
    for (int k = 0; k < total_bins; ++k) {
        freq_axis.bin_frequencies[k] = static_cast<float>(k) * bin_hz_max;
    }

    auto& time_axis = dataset.mutable_time_axis();
    time_axis.num_frames = num_frames;
    time_axis.hop_size = common_hop;
    time_axis.sample_rate = sample_rate;
    time_axis.frame_duration = static_cast<double>(common_hop) / static_cast<double>(sample_rate);
    time_axis.total_duration = meta.total_duration_seconds;
    time_axis.frame_times.resize(num_frames);
    for (int f = 0; f < num_frames; ++f) {
        time_axis.frame_times[f] = static_cast<double>(f) * time_axis.frame_duration;
    }

    // Merge all bands into merged magnitudes
    std::vector<std::vector<float>> merged_mag(num_frames, std::vector<float>(total_bins, 0.0f));
    std::vector<std::vector<float>> merged_pwr(num_frames, std::vector<float>(total_bins, 0.0f));
    for (const auto& br : band_results) {
        for (int f = 0; f < num_frames; ++f) {
            for (int k = br.band_start_bin; k <= br.band_end_bin; ++k) {
                if (br.magnitudes[f][k] > merged_mag[f][k]) {
                    merged_mag[f][k] = br.magnitudes[f][k];
                    merged_pwr[f][k] = br.power[f][k];
                }
            }
        }
    }

    // Add frames via add_frame
    for (int f = 0; f < num_frames; ++f) {
        SpectralFrame frame;
        frame.frame_index = f;
        frame.n_fft = max_nfft;
        frame.window_factor = 1.0f;
        frame.timestamp = time_axis.frame_times[f];
        frame.band_count = static_cast<int>(bands.size());
        frame.magnitudes = merged_mag[f];
        frame.power = merged_pwr[f];

        float sum_mag = 0;
        float max_mag = 0;
        int max_bin = 0;
        for (int k = 0; k < total_bins; ++k) {
            sum_mag += frame.magnitudes[k];
            if (frame.magnitudes[k] > max_mag) {
                max_mag = frame.magnitudes[k];
                max_bin = k;
            }
        }
        frame.rms = std::sqrt(sum_mag / static_cast<float>(total_bins));
        frame.peak_magnitude = max_mag;
        frame.spectral_centroid = (max_bin > 0) ?
            freq_axis.bin_frequencies[max_bin] : 0.0f;
        frame.spectral_bandwidth = 0.0f;

        dataset.add_frame(frame);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

    return { dataset, ms };
}

MultiBandAnalyzer::AnalyzeResult MultiBandAnalyzer::analyze_single(
    const std::vector<float>& samples,
    int sample_rate,
    int n_fft,
    int hop_size,
    const SpectralBackend* backend)
{
    auto t0 = std::chrono::high_resolution_clock::now();

    float nyquist = static_cast<float>(sample_rate) / 2.0f;
    int total_bins = n_fft / 2 + 1;
    int num_frames = static_cast<int>((static_cast<int>(samples.size()) - n_fft) / hop_size) + 1;
    if (num_frames < 1) num_frames = 1;
    float bin_hz = static_cast<float>(sample_rate) / static_cast<float>(n_fft);

    SpectralDataset dataset;

    auto& meta = dataset.mutable_analysis_metadata();
    meta.sample_rate = sample_rate;
    meta.fft_size = n_fft;
    meta.hop_size = hop_size;
    meta.overlap_ratio = static_cast<float>(n_fft - hop_size) / static_cast<float>(n_fft);
    meta.nyquist_frequency = nyquist;
    meta.num_frequency_bins = total_bins;
    meta.window_type = "hann";
    meta.window_coherent_gain = window_coherent_gain(window_hann(n_fft));
    meta.frame_duration_seconds = static_cast<double>(hop_size) / static_cast<double>(sample_rate);
    meta.total_duration_seconds = static_cast<double>(samples.size()) / static_cast<double>(sample_rate);
    meta.total_frames = num_frames;
    meta.analyzed_channels = 1;
    meta.analyzer_version = "2.0";
    meta.analysis_method = "stft";
    meta.band_count = 1;

    auto& freq_axis = dataset.mutable_frequency_axis();
    freq_axis.num_bins = total_bins;
    freq_axis.fft_size = n_fft;
    freq_axis.sample_rate = sample_rate;
    freq_axis.nyquist = nyquist;
    freq_axis.resolution = bin_hz;
    freq_axis.bin_frequencies.resize(total_bins);
    for (int k = 0; k < total_bins; ++k) {
        freq_axis.bin_frequencies[k] = static_cast<float>(k) * bin_hz;
    }

    auto& time_axis = dataset.mutable_time_axis();
    time_axis.num_frames = num_frames;
    time_axis.hop_size = hop_size;
    time_axis.sample_rate = sample_rate;
    time_axis.frame_duration = static_cast<double>(hop_size) / static_cast<double>(sample_rate);
    time_axis.total_duration = meta.total_duration_seconds;
    time_axis.frame_times.resize(num_frames);
    for (int f = 0; f < num_frames; ++f) {
        time_axis.frame_times[f] = static_cast<double>(f) * time_axis.frame_duration;
    }

    const auto& win = window_hann(n_fft);

    for (int f = 0; f < num_frames; ++f) {
        int offset = f * hop_size;
        if (offset + n_fft > static_cast<int>(samples.size())) break;

        std::vector<complex_f> buf(n_fft);
        for (int i = 0; i < n_fft; ++i) {
            buf[i] = complex_f(samples[offset + i] * win[i], 0.0f);
        }
        if (backend) {
            backend->fft(buf);
        } else {
            fft(buf);
        }

        auto [mag, pwr] = backend ? backend->fft_magnitude_power(buf) : fft_magnitude_power(buf);

        SpectralFrame frame;
        frame.frame_index = f;
        frame.n_fft = n_fft;
        frame.window_factor = 1.0f;
        frame.timestamp = time_axis.frame_times[f];
        frame.magnitudes = mag;
        frame.power = pwr;
        frame.band_count = 1;

        float sum_mag = 0;
        float max_mag = 0;
        int max_bin = 0;
        for (int k = 0; k < total_bins; ++k) {
            sum_mag += mag[k];
            if (mag[k] > max_mag) {
                max_mag = mag[k];
                max_bin = k;
            }
        }
        frame.rms = std::sqrt(sum_mag / static_cast<float>(total_bins));
        frame.peak_magnitude = max_mag;
        frame.spectral_centroid = (max_bin > 0) ?
            freq_axis.bin_frequencies[max_bin] : 0.0f;
        frame.spectral_bandwidth = 0.0f;

        dataset.add_frame(frame);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

    return { dataset, ms };
}

} // namespace Spectral
