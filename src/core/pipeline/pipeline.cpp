#include "pipeline.h"

#include "fft.h"
#include "stft.h"
#include "media_decoder.h"
#include "spectrogram_renderer.h"
#include "spectrum_renderer.h"
#include "png_encoder.h"
#include "spectral_dataset.h"
#include "video_renderer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace Spectral {

static void report(const ProgressFn& p, float f, const char* stage) {
    if (p) p(f, stage);
}

static bool is_supported_window(const std::string& type) {
    return type == "hann" || type == "hamming" || type == "blackman" ||
           type == "rectangular";
}

std::string validate_config(const GenerateConfig& cfg) {
    if (cfg.input_path.empty()) return "no input file";
    if (cfg.output_path.empty()) return "no output file";
    if (cfg.visualization != "spectrogram" && cfg.visualization != "spectrum")
        return "visualization must be spectrogram or spectrum";
    if (cfg.fft_size < 2 || (cfg.fft_size & (cfg.fft_size - 1)) != 0)
        return "fft must be a power of 2 >= 2";
    if (!is_supported_window(cfg.window))
        return "invalid window: '" + cfg.window +
               "'. supported windows: hann, hamming, blackman, rectangular";
    // hop == 0 means fft_size / 2 (normalized by callers). Anything else
    // must be an explicit positive step within one window: negative hops
    // would run the STFT loop backwards, and hops past fft_size would
    // silently skip input between windows. Neither is supported.
    if (cfg.hop_size < 0)
        return "hop-size must be >= 0 (0 = fft-size / 2)";
    if (cfg.hop_size > cfg.fft_size)
        return "hop-size must be <= fft-size (gapped windows unsupported)";
    if (cfg.width <= 0 || cfg.height <= 0) return "resolution must be positive";
    if (cfg.db_range <= 0) return "db-range must be > 0";
    if (cfg.output_format != "image" && cfg.output_format != "video")
        return "output-format must be image or video";
    if (cfg.max_freq > 0.0f && cfg.min_freq >= cfg.max_freq)
        return "min-frequency must be below max-frequency";
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

// Fail-closed: unknown names yield an empty window, which stft_frame
// rejects. Unreachable via validate_config, but no silent Hann fallback.
static std::vector<float> make_window(const std::string& type, int n) {
    if (type == "hamming")     return window_hamming(n);
    if (type == "blackman")    return window_blackman(n);
    if (type == "rectangular") return window_rectangular(n);
    if (type == "hann")        return window_hann(n);
    return {};
}

Error analyze_dataset(const GenerateConfig& cfg_in, SpectralDataset& dataset,
                         std::vector<float>& samples_out, ProgressFn progress) {
    GenerateConfig cfg = cfg_in;
    const std::string cfg_err = validate_config(cfg);
    if (!cfg_err.empty())
        return Error::make(Subsystem::Pipeline, JobError::BadConfig, "config: " + cfg_err);
    if (cfg.hop_size == 0) cfg.hop_size = cfg.fft_size / 2;

    report(progress, 0.0f, "decode");
    MediaDecoder decoder;
    if (!decoder.open(cfg.input_path)) {
        std::string why = decoder.last_error();
        if (why.empty()) why = "media: cannot open '" + cfg.input_path + "'";
        return Error::make(Subsystem::Media, JobError::FileNotFound, why);
    }
    int sr = decoder.sample_rate();
    if (sr <= 0)
        return Error::make(Subsystem::Media, JobError::DecodeError,
                           "media: no valid sample rate from '" + cfg.input_path + "'");

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
    const bool decode_failed = decoder.failed();
    const std::string decode_err = decoder.last_error();
    decoder.close();
    // A failed decode is never a silent success, even with partial audio.
    if (decode_failed) {
        std::string why = decode_err.empty()
                                ? "media: decode failed for '" + cfg.input_path + "'"
                                : decode_err;
        return Error::make(Subsystem::Media, JobError::DecodeError, why);
    }
    if (audio.empty())
        return Error::make(Subsystem::Media, JobError::DecodeError,
                           "media: decoded 0 audio samples from '" + cfg.input_path + "'");

    // STFT
    report(progress, 0.1f, "analyze");
    dataset.mutable_frequency_axis() = FrequencyAxis(cfg.fft_size, sr);
    std::vector<float> win = make_window(cfg.window, cfg.fft_size);
    float cg = window_coherent_gain(win);
    const int num_bins = cfg.fft_size / 2 + 1;
    const int total = static_cast<int>(audio.size());
    const int n_frames = (total - cfg.fft_size) / cfg.hop_size + 1;
    if (n_frames <= 0)
        return Error::make(Subsystem::Dsp, JobError::AnalysisError,
                           "dsp: input too short for fft_size=" + std::to_string(cfg.fft_size));

    int frame_idx = 0;
    for (int start = 0; start + cfg.fft_size <= total; start += cfg.hop_size) {
        // Authoritative STFT: amplitude-corrected one-sided magnitudes.
        StftFrame fr;
        if (!stft_frame(audio.data(), total, start, cfg.fft_size, sr, win, cg, fr))
            return Error::make(Subsystem::Dsp, JobError::AnalysisError,
                               "dsp: stft frame failed at sample " + std::to_string(start));
        std::vector<complex_f>& buf = fr.spectrum;

        SpectralFrame sf;
        sf.frame_index = frame_idx;
        sf.n_fft = cfg.fft_size;
        sf.window_factor = cg;
        sf.timestamp = fr.timestamp;
        sf.magnitudes = std::move(fr.magnitudes);
        sf.phases = std::move(fr.phases);
        sf.power = std::move(fr.power);

        float sum_sq = 0.0f, peak = 0.0f, cn = 0.0f, cd = 0.0f;
        for (int k = 0; k < num_bins; ++k) {
            float mag = sf.magnitudes[static_cast<size_t>(k)];
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

    return Error::success();
}

// ponytail: temp + rename so interrupted jobs never leave partial outputs.
// Overwrite = remove + rename (fs::rename fails on existing Windows targets).
static bool commit_file(const std::string& tmp, const std::string& dst) {
    std::error_code ec;
    // ponytail: never touch an existing directory — fail clearly instead
    if (fs::is_directory(dst, ec) && !ec) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::remove(dst, ec);
    fs::rename(tmp, dst, ec);
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

// ponytail: keep the real extension — ffmpeg sniffs format from it
static std::string tmp_for(const std::string& dst) {
    fs::path p(dst);
    fs::path tmp = p;
    tmp.replace_filename(p.stem().string() + ".part" + p.extension().string());
    return tmp.string();
}

Error render_dataset(const GenerateConfig& cfg_in, const SpectralDataset& dataset) {
    GenerateConfig cfg = cfg_in;
    const std::string render_cfg_err = validate_config(cfg);
    if (!render_cfg_err.empty())
        return Error::make(Subsystem::Pipeline, JobError::BadConfig, "config: " + render_cfg_err);
    if (cfg.hop_size == 0) cfg.hop_size = cfg.fft_size / 2;
    const float fmin = (cfg.min_freq > 0) ? cfg.min_freq : 20.0f;
    const std::string tmp = tmp_for(cfg.output_path);
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
        if (vrend.render(dataset, tmp) != VideoRenderError::Ok)
            return Error::make(Subsystem::Encode, JobError::RenderError,
                               "encode: video render failed for '" + cfg.output_path + "'");
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
                return Error::make(Subsystem::Render, JobError::RenderError,
                                   "render: spectrogram (GPU) failed for '" + cfg.output_path + "'");
            if (!PNGEncoder::write_rgba(tmp, img.width, img.height, img.pixels.data()))
                return Error::make(Subsystem::Render, JobError::RenderError,
                                   "render: PNG write failed for '" + tmp + "'");
        } else {
            if (renderer.render_to_png(dataset, tmp) != RenderError::Ok)
                return Error::make(Subsystem::Render, JobError::RenderError,
                                   "render: spectrogram failed for '" + cfg.output_path + "'");
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
        if (renderer.render_to_png(dataset, tmp) != SpectrumError::Ok)
            return Error::make(Subsystem::Render, JobError::RenderError,
                               "render: spectrum failed for '" + cfg.output_path + "'");
    }

    if (!commit_file(tmp, cfg.output_path))
        return Error::make(Subsystem::Pipeline, JobError::RenderError,
                           "pipeline: cannot commit output '" + cfg.output_path + "'");
    return Error::success();
}

static bool cancelled(const std::atomic<bool>* c) { return c && c->load(); }

Error run_job(const GenerateConfig& cfg, ProgressFn progress,
                 const std::atomic<bool>* cancel) {
    try {
        SpectralDataset dataset;
        std::vector<float> samples;
        Error err = analyze_dataset(cfg, dataset, samples, progress);
        if (!err.ok()) return err;
        if (cancelled(cancel))
            return Error::make(Subsystem::Pipeline, JobError::AnalysisError, "pipeline: cancelled");
        report(progress, 0.65f, "render");
        err = render_dataset(cfg, dataset);
        if (err != JobError::Ok) return err;
        report(progress, 1.0f, "done");
        return Error::success();
    } catch (const std::bad_alloc&) {
        // very long/large files: fail clearly
        return Error::make(Subsystem::Pipeline, JobError::AnalysisError, "pipeline: out of memory");
    } catch (const std::exception& ex) {
        return Error::make(Subsystem::Pipeline, JobError::AnalysisError,
                           std::string("pipeline: ") + ex.what());
    } catch (...) {
        return Error::make(Subsystem::Pipeline, JobError::AnalysisError, "pipeline: unknown failure");
    }
}

} // namespace Spectral
