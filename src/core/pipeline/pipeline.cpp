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

static FrequencyScale parse_video_freq_scale(const std::string& s) {
    if (s == "mel")    return FrequencyScale::Mel;
    if (s == "bark")   return FrequencyScale::Bark;
    if (s == "erb")    return FrequencyScale::Erb;
    if (s == "cqt")    return FrequencyScale::CQT;
    if (s == "linear") return FrequencyScale::Linear;
    return FrequencyScale::Logarithmic;
}

// Canonical window vector from the canonical enum (single source for
// both analysis and recorded gains). No string fallback anywhere.
// Fail-closed: any value outside the four known enumerators yields an
// empty window. Callers treat empty as rejection (stft_frame refuses it;
// analyze_dataset returns BadConfig). No Hann fallback.
static std::vector<float> window_for(ProjectWindowType w, int n) {
    switch (w) {
        case ProjectWindowType::Hamming:     return window_hamming(n);
        case ProjectWindowType::Blackman:    return window_blackman(n);
        case ProjectWindowType::Rectangular: return window_rectangular(n);
        case ProjectWindowType::Hann:        return window_hann(n);
        default:                             return {};
    }
}

static const char* channel_name_for(int ch) {
    static const char* names[] = {"L", "R", "C", "LFE", "SL", "SR", "BL", "BR"};
    return (ch >= 0 && ch < 8) ? names[ch] : "?";
}

ProjectConfig make_project_config(const GenerateConfig& cfg, const DecodedMedia& media) {
    ProjectConfig pc;
    const int hop = cfg.hop_size == 0 ? cfg.fft_size / 2 : cfg.hop_size;
    pc.input.file_path = media.file_path;
    pc.input.file_hash = media.file_hash;
    pc.input.file_size_bytes = media.file_size_bytes;
    pc.input.sample_rate = media.sample_rate;
    pc.input.num_channels = media.num_channels;
    pc.input.duration_seconds = media.duration_seconds;
    pc.input.codec_name = media.codec_name;
    ProjectWindowType wt = ProjectWindowType::Hann;
    try_parse_window_type(cfg.window, wt);  // validated upstream
    pc.analysis.window_type = wt;
    const auto win = window_for(wt, cfg.fft_size);
    pc.analysis.window_coherent_gain =
        win.empty() ? 0.5f : window_coherent_gain(win);
    pc.analysis.fft_size = cfg.fft_size;
    pc.analysis.hop_size = hop;
    pc.analysis.overlap_ratio =
        cfg.fft_size > 0 ? 1.0f - static_cast<float>(hop) / cfg.fft_size : 0.5f;
    pc.analysis.sample_rate = media.sample_rate;
    pc.analysis.analyzed_channels = 1;
    pc.analysis.channel_mapping = 0;
    pc.dynamic_range.db_floor = -cfg.db_range;
    pc.dynamic_range.db_ceiling = 0.0f;
    pc.dynamic_range.window_energy_gain =
        win.empty() ? 0.0f : window_energy_gain(win);
    pc.frequency_range.min_hz = cfg.min_freq;
    pc.frequency_range.max_hz = cfg.max_freq;
    ProjectFreqScale sc = ProjectFreqScale::Logarithmic;
    try_parse_freq_scale(cfg.freq_scale, sc);  // unknown keeps Logarithmic, as before
    pc.frequency_range.scale = sc;
    // Representation produced here is always conventional STFT today.
    // Bins stay implied by fft_size; future Mel/CQT builders will set
    // explicit counts through this same struct (never N/2+1 for them).
    pc.analysis.representation = RepresentationInfo::stft_default();
    pc.analysis.representation.reassignment_supported = cfg.reassigned;
    pc.renderer.kind = (cfg.visualization == "spectrogram") ? RendererKind::Spectrogram
                                                              : RendererKind::Spectrum;
    pc.renderer.width = cfg.width;
    pc.renderer.height = cfg.height;
    pc.renderer.cqt_center_hz = cfg.cqt_center;
    pc.renderer.cqt_q = cfg.cqt_q;
    return pc;
}

Error analyze_dataset(const GenerateConfig& cfg_in, SpectralDataset& dataset,
                         std::vector<float>& samples_out, ProgressFn progress) {
    GenerateConfig cfg = cfg_in;
    const std::string cfg_err = validate_config(cfg);
    if (!cfg_err.empty())
        return Error::make(Subsystem::Pipeline, JobError::BadConfig, "config: " + cfg_err);
    // Hop defaulting lives in make_project_config (single site).

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
    const int native_ch = decoder.num_channels();
    const std::string codec = decoder.codec_name();
    const double dec_duration = decoder.duration();
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

    // Content identity: one chunked hash per job. Same bytes under any
    // path hash identically; the hash (not the path) feeds the dataset.
    std::string content_hash, hash_err;
    uint64_t content_size = 0;
    {
        std::error_code fec;
        content_size = fs::file_size(cfg.input_path, fec);
    }
    if (!sha256_file(cfg.input_path, content_hash, hash_err))
        return Error::make(Subsystem::Media, JobError::DecodeError,
                           "media: cannot hash '" + cfg.input_path + "': " + hash_err);
    DecodedMedia media;
    media.file_path = cfg.input_path;
    media.file_hash = content_hash;
    media.file_size_bytes = content_size;
    media.sample_rate = sr;
    media.num_channels = native_ch;
    media.duration_seconds = dec_duration;
    media.codec_name = codec;
    const ProjectConfig pc = make_project_config(cfg, media);
    {
        std::vector<std::string> verr;
        if (!pc.validate(verr))
            return Error::make(Subsystem::Pipeline, JobError::BadConfig,
                               "config: " + (verr.empty() ? "invalid" : verr[0]));
    }

    // STFT (parameters from the canonical config, not parallel locals).
    report(progress, 0.1f, "analyze");
    const int fft_n = pc.analysis.fft_size;
    const int hop_n = pc.analysis.hop_size;
    dataset.mutable_frequency_axis() = FrequencyAxis(fft_n, sr);
    std::vector<float> win = window_for(pc.analysis.window_type, fft_n);
    if (win.empty())
        return Error::make(Subsystem::Pipeline, JobError::BadConfig,
                           "config: unsupported window type");
    float cg = pc.analysis.window_coherent_gain;
    const int num_bins = fft_n / 2 + 1;
    const int total = static_cast<int>(audio.size());
    const int n_frames = (total - fft_n) / hop_n + 1;
    if (n_frames <= 0)
        return Error::make(Subsystem::Dsp, JobError::AnalysisError,
                           "dsp: input too short for fft_size=" + std::to_string(fft_n));

    int frame_idx = 0;
    for (int start = 0; start + fft_n <= total; start += hop_n) {
        // Authoritative STFT: amplitude-corrected one-sided magnitudes.
        StftFrame fr;
        if (!stft_frame(audio.data(), total, start, fft_n, sr, win, cg, fr))
            return Error::make(Subsystem::Dsp, JobError::AnalysisError,
                               "dsp: stft frame failed at sample " + std::to_string(start));
        std::vector<complex_f>& buf = fr.spectrum;

        SpectralFrame sf;
        sf.frame_index = frame_idx;
        sf.n_fft = fft_n;
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
            float freq = static_cast<float>(k) * static_cast<float>(sr) / static_cast<float>(fft_n);
            cn += freq * mag;
            cd += mag;
        }
        sf.rms = std::sqrt(sum_sq / static_cast<float>(num_bins));
        sf.peak_magnitude = peak;
        sf.spectral_centroid = (cd > 0.0f) ? cn / cd : 0.0f;

        if (cfg.reassigned) {
            std::vector<complex_f> X_tau(static_cast<size_t>(fft_n));
            for (int j = 0; j < fft_n; ++j)
                X_tau[static_cast<size_t>(j)] = complex_f(
                    static_cast<float>(j) * win[static_cast<size_t>(j)] *
                    audio[static_cast<size_t>(start + j)], 0.0f);
            fft(X_tau);
            std::vector<float> w_deriv(static_cast<size_t>(fft_n));
            for (int j = 0; j < fft_n; ++j)
                w_deriv[static_cast<size_t>(j)] = static_cast<float>(PI) /
                    static_cast<float>(fft_n - 1) *
                    std::sin(2.0f * PI * static_cast<float>(j) /
                             static_cast<float>(fft_n - 1));
            std::vector<complex_f> X_dg(static_cast<size_t>(fft_n));
            for (int j = 0; j < fft_n; ++j)
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
                        static_cast<float>(k) * static_cast<float>(sr) / static_cast<float>(fft_n) +
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

    // Dataset metadata comes from the canonical config — never from
    // parallel locals. Analysis method/version record the S4 math.
    dataset.mutable_time_axis() = TimeAxis(dataset.frame_count(), hop_n, sr);
    {
        auto& sm = dataset.mutable_source_metadata();
        sm.file_path = pc.input.file_path;
        sm.file_hash = pc.input.file_hash;
        sm.file_size_bytes = pc.input.file_size_bytes;
        sm.sample_rate = pc.input.sample_rate;
        sm.num_channels = pc.input.num_channels;
        sm.duration_seconds = pc.input.duration_seconds;
        sm.codec_name = pc.input.codec_name;
    }
    {
        auto& am = dataset.mutable_analysis_metadata();
        ProjectConfigAdapter::to_analysis_metadata(pc, am);
        am.analyzer_version = "2";  // S4 amplitude-corrected one-sided STFT
        am.total_frames = dataset.frame_count();
        am.total_duration_seconds = dataset.total_duration();
    }
    {
        auto& ci = dataset.mutable_channel_info();
        ci.total_channels = native_ch;
        ci.analyzed_channels = 1;
        ci.analyzed_channel_index = 0;
        ci.channels_mixed = (native_ch > 1);
        ci.channel_names.clear();
        for (int ch = 0; ch < native_ch && ch < 64; ++ch)
            ci.channel_names.push_back(channel_name_for(ch));
    }
    {
        // Dataset representation mirrors the canonical config, made
        // explicit: STFT bins are the produced count, range the Nyquist
        // span, phase available, reassignment per the request.
        auto& rep = dataset.mutable_representation();
        rep = pc.analysis.representation;
        rep.bins = dataset.num_frequency_bins();
        rep.fmin_hz = 0.0f;
        rep.fmax_hz = dataset.nyquist_frequency();
        rep.phase = RepresentationPhase::Available;
        auto& nm = dataset.mutable_normalization_info();
        nm.window_coherent_gain = pc.analysis.window_coherent_gain;
        nm.window_energy_gain = pc.dynamic_range.window_energy_gain;
        nm.magnitude_scale = pc.analysis.magnitude_scale;
        nm.reference_amplitude = pc.dynamic_range.reference_amplitude;
        nm.db_floor = pc.dynamic_range.db_floor;
        nm.db_reference = pc.dynamic_range.reference_amplitude;
    }
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
    // Hop defaulting lives in make_project_config (single site).
    // Render configuration flows from the canonical ProjectConfig through
    // the adapter (single mapping). Pipeline execution defaults that the
    // canonical model intentionally leaves open: min_hz 0 = 20 Hz here.
    // Video-only fields (fps/codec/crf) stay on GenerateConfig: the video
    // tier is explicitly not byte-reproducible.
    DecodedMedia rmedia;
    rmedia.file_path = dataset.source_metadata().file_path;
    rmedia.file_hash = dataset.source_metadata().file_hash;
    rmedia.file_size_bytes = dataset.source_metadata().file_size_bytes;
    rmedia.sample_rate = dataset.sample_rate();
    rmedia.num_channels = dataset.channel_info().total_channels;
    rmedia.duration_seconds = dataset.source_metadata().duration_seconds;
    rmedia.codec_name = dataset.source_metadata().codec_name;
    const ProjectConfig rpc = make_project_config(cfg, rmedia);
    const float fmin = (cfg.min_freq > 0) ? cfg.min_freq : 20.0f;
    const std::string tmp = tmp_for(cfg.output_path);
    if (cfg.output_format == "video") {
        VideoRendererConfig vrcfg;
        vrcfg.width = cfg.width;
        vrcfg.height = cfg.height;
        vrcfg.fps = cfg.fps;
        vrcfg.codec = cfg.video_codec;
        vrcfg.crf = cfg.crf;
        vrcfg.freq_scale = parse_video_freq_scale(cfg.freq_scale);
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
        ProjectConfigAdapter::to_spectrogram_config(rpc, sc);
        sc.freq_min_hz = fmin;
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
        ProjectConfigAdapter::to_spectrum_config(rpc, sc);
        sc.freq_min_hz = fmin;
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
