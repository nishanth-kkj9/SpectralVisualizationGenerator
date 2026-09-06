#include "pipeline.h"

#include "fft.h"
#include "media_decoder.h"
#include "spectrogram_renderer.h"
#include "spectrum_renderer.h"
#include "png_encoder.h"
#include "spectral_dataset.h"
#include "video_renderer.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace Spectral {

static void report(const ProgressFn& p, float f, const char* stage) {
    if (p) p(f, stage);
}

std::string validate_config(const GenerateConfig& cfg) {
    if (cfg.input_path.empty()) return "no input file";
    if (cfg.output_path.empty()) return "no output file";
    if (cfg.visualization != "spectrogram" && cfg.visualization != "spectrum")
        return "visualization must be spectrogram or spectrum";
    if (cfg.fft_size <= 0 || (cfg.fft_size & (cfg.fft_size - 1)) != 0)
        return "fft must be a positive power of 2";
    if (cfg.width <= 0 || cfg.height <= 0) return "resolution must be positive";
    if (cfg.db_range <= 0) return "db-range must be > 0";
    if (cfg.output_format != "image" && cfg.output_format != "video")
        return "output-format must be image or video";
    if (cfg.fps <= 0 || cfg.fps > 120) return "fps must be 1..120";
    if (cfg.crf < 0 || cfg.crf > 51) return "crf must be 0..51";
    return "";
}

static FrequencyScale parse_freq_scale(const std::string& s) {
    if (s == "mel")    return FrequencyScale::Mel;
    if (s == "bark")   return FrequencyScale::Bark;
    if (s == "erb")    return FrequencyScale::Erb;
    if (s == "cqt")    return FrequencyScale::CQT;
    if (s == "linear") return FrequencyScale::Linear;
    return FrequencyScale::Logarithmic;
}

static std::vector<float> make_window(const std::string& type, int n) {
    if (type == "hamming")     return window_hamming(n);
    if (type == "blackman")    return window_blackman(n);
    if (type == "rectangular") return window_rectangular(n);
    return window_hann(n);
}

JobError analyze_dataset(const GenerateConfig& cfg_in, SpectralDataset& dataset,
                         std::vector<float>& samples_out, ProgressFn progress) {
    GenerateConfig cfg = cfg_in;
    if (!validate_config(cfg).empty()) return JobError::BadConfig;
    if (cfg.hop_size == 0) cfg.hop_size = cfg.fft_size / 2;

    report(progress, 0.0f, "decode");
    MediaDecoder decoder;
    if (!decoder.open(cfg.input_path)) return JobError::FileNotFound;
    int sr = decoder.sample_rate();
    if (sr <= 0) return JobError::DecodeError;

    std::vector<float> audio;
    AudioFrame frame;
    while (decoder.read_frame(frame)) {
        if (frame.num_channels == 1) {
            audio.insert(audio.end(), frame.samples.begin(), frame.samples.end());
        } else {
            size_t n = frame.samples.size() / static_cast<size_t>(frame.num_channels);
            audio.reserve(audio.size() + n);
            for (size_t i = 0; i < n; ++i) {
                float sum = 0.0f;
                for (int ch = 0; ch < frame.num_channels; ++ch)
                    sum += frame.samples[i * static_cast<size_t>(frame.num_channels) + ch];
                audio.push_back(sum / static_cast<float>(frame.num_channels));
            }
        }
    }
    decoder.close();
    if (audio.empty()) return JobError::DecodeError;

    // STFT
    report(progress, 0.1f, "analyze");
    dataset.mutable_frequency_axis() = FrequencyAxis(cfg.fft_size, sr);
    std::vector<float> win = make_window(cfg.window, cfg.fft_size);
    float cg = window_coherent_gain(win);
    const int num_bins = cfg.fft_size / 2 + 1;
    const int total = static_cast<int>(audio.size());
    const int n_frames = (total - cfg.fft_size) / cfg.hop_size + 1;
    if (n_frames <= 0) return JobError::AnalysisError;

    int frame_idx = 0;
    for (int start = 0; start + cfg.fft_size <= total; start += cfg.hop_size) {
        std::vector<complex_f> buf(static_cast<size_t>(cfg.fft_size));
        for (int j = 0; j < cfg.fft_size; ++j)
            buf[static_cast<size_t>(j)] = complex_f(
                audio[static_cast<size_t>(start + j)] * win[static_cast<size_t>(j)], 0.0f);
        fft(buf);

        SpectralFrame sf;
        sf.frame_index = frame_idx;
        sf.n_fft = cfg.fft_size;
        sf.window_factor = cg;
        sf.timestamp = static_cast<double>(start) / static_cast<double>(sr);
        sf.magnitudes.resize(static_cast<size_t>(num_bins));
        sf.phases.resize(static_cast<size_t>(num_bins));
        sf.power.resize(static_cast<size_t>(num_bins));

        float scale = 1.0f / static_cast<float>(cfg.fft_size);
        float sum_sq = 0.0f, peak = 0.0f, cn = 0.0f, cd = 0.0f;
        for (int k = 0; k < num_bins; ++k) {
            float re = buf[static_cast<size_t>(k)].real();
            float im = buf[static_cast<size_t>(k)].imag();
            float mag = std::sqrt(re * re + im * im) * scale;
            sf.magnitudes[static_cast<size_t>(k)] = mag;
            sf.phases[static_cast<size_t>(k)] = std::atan2(im, re);
            sf.power[static_cast<size_t>(k)] = mag * mag;
            sum_sq += mag * mag;
            if (mag > peak) peak = mag;
            float freq = static_cast<float>(k) * static_cast<float>(sr) / static_cast<float>(cfg.fft_size);
            cn += freq * mag;
            cd += mag;
        }
        sf.rms = std::sqrt(sum_sq / static_cast<float>(num_bins));
        sf.peak_magnitude = peak;
        sf.spectral_centroid = (cd > 0.0f) ? cn / cd : 0.0f;

        if (cfg.reassigned) {
            std::vector<complex_f> X_tau(static_cast<size_t>(cfg.fft_size));
            for (int j = 0; j < cfg.fft_size; ++j)
                X_tau[static_cast<size_t>(j)] = complex_f(
                    static_cast<float>(j) * win[static_cast<size_t>(j)] *
                    audio[static_cast<size_t>(start + j)], 0.0f);
            fft(X_tau);
            std::vector<float> w_deriv(static_cast<size_t>(cfg.fft_size));
            for (int j = 0; j < cfg.fft_size; ++j)
                w_deriv[static_cast<size_t>(j)] = static_cast<float>(PI) /
                    static_cast<float>(cfg.fft_size - 1) *
                    std::sin(2.0f * PI * static_cast<float>(j) /
                             static_cast<float>(cfg.fft_size - 1));
            std::vector<complex_f> X_dg(static_cast<size_t>(cfg.fft_size));
            for (int j = 0; j < cfg.fft_size; ++j)
                X_dg[static_cast<size_t>(j)] = complex_f(
                    w_deriv[static_cast<size_t>(j)] * audio[static_cast<size_t>(start + j)], 0.0f);
            fft(X_dg);
            sf.reassigned_times.resize(static_cast<size_t>(num_bins));
            sf.reassigned_freqs.resize(static_cast<size_t>(num_bins));
            for (int k = 0; k < num_bins; ++k) {
                float re = buf[static_cast<size_t>(k)].real();
                float im = buf[static_cast<size_t>(k)].imag();
                float mag_sq = re * re + im * im;
                if (mag_sq > 1e-12f) {
                    complex_f conj_X(re, -im);
                    float corr = (conj_X * X_dg[static_cast<size_t>(k)]).imag();
                    sf.reassigned_freqs[static_cast<size_t>(k)] =
                        static_cast<float>(k) * static_cast<float>(sr) / static_cast<float>(cfg.fft_size) +
                        corr / (2.0f * PI * mag_sq) * static_cast<float>(sr);
                    float dot_tau = (conj_X * X_tau[static_cast<size_t>(k)]).real();
                    sf.reassigned_times[static_cast<size_t>(k)] = dot_tau / mag_sq / static_cast<float>(sr);
                } else {
                    sf.reassigned_freqs[static_cast<size_t>(k)] = 0.0f;
                    sf.reassigned_times[static_cast<size_t>(k)] = 0.0f;
                }
            }
        }

        dataset.add_frame(sf);
        ++frame_idx;
        if ((frame_idx & 31) == 0)
            report(progress, 0.1f + 0.5f * frame_idx / n_frames, "analyze");
    }

    dataset.mutable_time_axis() = TimeAxis(dataset.frame_count(), cfg.hop_size, sr);
    dataset.mutable_source_metadata().file_path = cfg.input_path;
    auto& am = dataset.mutable_analysis_metadata();
    am.fft_size = cfg.fft_size;
    am.hop_size = cfg.hop_size;
    am.sample_rate = sr;
    samples_out = audio;

    return JobError::Ok;
}

JobError render_dataset(const GenerateConfig& cfg_in, const SpectralDataset& dataset) {
    GenerateConfig cfg = cfg_in;
    if (!validate_config(cfg).empty()) return JobError::BadConfig;
    if (cfg.hop_size == 0) cfg.hop_size = cfg.fft_size / 2;
    const float fmin = (cfg.min_freq > 0) ? cfg.min_freq : 20.0f;
    if (cfg.output_format == "video") {
        VideoRendererConfig vrcfg;
        vrcfg.width = cfg.width;
        vrcfg.height = cfg.height;
        vrcfg.fps = cfg.fps;
        vrcfg.codec = cfg.video_codec;
        vrcfg.crf = cfg.crf;
        vrcfg.freq_scale = parse_freq_scale(cfg.freq_scale);
        vrcfg.freq_min_hz = fmin;
        vrcfg.freq_max_hz = cfg.max_freq;
        vrcfg.db_floor = -cfg.db_range;
        vrcfg.db_ceiling = 0.0f;
        vrcfg.window_seconds = cfg.window_seconds;
        VideoRenderer vrend(vrcfg);
        if (vrend.render(dataset, cfg.output_path) != VideoRenderError::Ok)
            return JobError::RenderError;
    } else if (cfg.visualization == "spectrogram") {
        SpectrogramConfig sc;
        sc.width = cfg.width;
        sc.height = cfg.height;
        sc.freq_scale = parse_freq_scale(cfg.freq_scale);
        sc.freq_min_hz = fmin;
        sc.freq_max_hz = cfg.max_freq;
        sc.cqt_center_hz = cfg.cqt_center;
        sc.cqt_q = cfg.cqt_q;
        sc.db_ceiling = 0.0f;
        sc.db_floor = -cfg.db_range;
        SpectrogramRenderer renderer(sc);
        if (cfg.use_gpu) {
            // ponytail: GPU fills RGBAImage; same PNG writer as CPU path
            RGBAImage img;
            if (renderer.render_gpu(dataset, img) != RenderError::Ok)
                return JobError::RenderError;
            if (!PNGEncoder::write_rgba(cfg.output_path, img.width, img.height,
                                        img.pixels.data()))
                return JobError::RenderError;
        } else {
            if (renderer.render_to_png(dataset, cfg.output_path) != RenderError::Ok)
                return JobError::RenderError;
        }
    } else {
        SpectrumConfig sc;
        sc.width = cfg.width;
        sc.height = cfg.height;
        sc.freq_scale = parse_freq_scale(cfg.freq_scale);
        sc.freq_min_hz = fmin;
        sc.freq_max_hz = cfg.max_freq;
        sc.cqt_center_hz = cfg.cqt_center;
        sc.cqt_q = cfg.cqt_q;
        sc.db_ceiling = 0.0f;
        sc.db_floor = -cfg.db_range;
        SpectrumRenderer renderer(sc);
        if (renderer.render_to_png(dataset, cfg.output_path) != SpectrumError::Ok)
            return JobError::RenderError;
    }

    return JobError::Ok;
}

JobError run_job(const GenerateConfig& cfg, ProgressFn progress) {
    SpectralDataset dataset;
    std::vector<float> samples;
    JobError err = analyze_dataset(cfg, dataset, samples, progress);
    if (err != JobError::Ok) return err;
    report(progress, 0.65f, "render");
    err = render_dataset(cfg, dataset);
    if (err != JobError::Ok) return err;
    report(progress, 1.0f, "done");
    return JobError::Ok;
}

} // namespace Spectral
