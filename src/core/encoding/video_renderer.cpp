#include "video_renderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Spectral {

// Map a colormap name string to enum
static ColorMap parse_color_map(const std::string& name) {
    if (name == "heat") return ColorMap::Heat;
    return ColorMap::Viridis;
}

VideoRenderError VideoRenderer::render_frame(const SpectralDataset& dataset,
                                             double time_sec,
                                             RGBAImage& out) const {
    out.clear();
    if (dataset.frame_count() == 0) return VideoRenderError::EmptyDataset;

    const int W = cfg_.width;
    const int H = cfg_.height;
    const float total_dur = dataset.total_duration();
    const float win_sec = cfg_.window_seconds;

    // Determine the time window [t_start, t_end] centered on time_sec
    float t_start = static_cast<float>(time_sec) - win_sec * 0.5f;
    float t_end = t_start + win_sec;
    if (t_start < 0.0f) { t_start = 0.0f; t_end = win_sec; }
    if (t_end > total_dur) { t_end = total_dur; t_start = std::max(0.0f, t_end - win_sec); }

    // Find spectral frames within the window
    const int Nf = dataset.frame_count();

    // Binary search for first frame with timestamp >= t_start
    int idx_lo = 0;
    {
        int lo = 0, hi = Nf;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (dataset.frame(mid).timestamp < t_start) lo = mid + 1;
            else hi = mid;
        }
        idx_lo = lo;
    }

    // Binary search for last frame with timestamp <= t_end
    int idx_hi = Nf - 1;
    {
        int lo = idx_lo, hi = Nf;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (dataset.frame(mid).timestamp <= t_end) lo = mid + 1;
            else hi = mid;
        }
        idx_hi = lo - 1;
    }

    // Clamp
    if (idx_lo >= Nf) idx_lo = Nf - 1;
    if (idx_hi < idx_lo) idx_hi = idx_lo;

    // Build a subset dataset with timestamps shifted to [0, win_sec]
    SpectralDataset subset;
    subset.mutable_frequency_axis() = dataset.frequency_axis();

    const double t_offset = t_start;
    for (int i = idx_lo; i <= idx_hi; ++i) {
        const auto& src_frame = dataset.frame(i);
        SpectralFrame sf = src_frame;
        sf.timestamp = src_frame.timestamp - t_offset;
        sf.frame_index = i - idx_lo;
        subset.add_frame(sf);
    }

    // Set time axis for the subset
    if (subset.frame_count() > 0) {
        subset.mutable_time_axis() = TimeAxis(
            subset.frame_count(),
            dataset.hop_size(),
            dataset.sample_rate()
        );
    }

    subset.mutable_analysis_metadata() = dataset.analysis_metadata();

    // Render using SpectrogramRenderer
    SpectrogramConfig sc;
    sc.width = W;
    sc.height = H;
    sc.color_map = parse_color_map(cfg_.color_map_name);
    sc.freq_scale = cfg_.freq_scale;
    sc.freq_min_hz = cfg_.freq_min_hz;
    sc.freq_max_hz = cfg_.freq_max_hz;
    sc.db_floor = cfg_.db_floor;
    sc.db_ceiling = cfg_.db_ceiling;

    SpectrogramRenderer renderer(sc);
    auto err = renderer.render(subset, out);
    if (err != RenderError::Ok) return VideoRenderError::EmptyDataset;

    return VideoRenderError::Ok;
}

VideoRenderError VideoRenderer::render(const SpectralDataset& dataset,
                                       const std::string& output_path) const {
    if (dataset.frame_count() == 0) return VideoRenderError::EmptyDataset;

    // Open encoder
    VideoEncoder encoder;
    VideoEncoderConfig enc_cfg;
    enc_cfg.width = cfg_.width;
    enc_cfg.height = cfg_.height;
    enc_cfg.fps = cfg_.fps;
    enc_cfg.codec = cfg_.codec;
    enc_cfg.crf = cfg_.crf;

    auto enc_err = encoder.open(output_path, enc_cfg);
    if (enc_err != VideoEncoderError::Ok) return VideoRenderError::EncoderOpenFailed;

    // Calculate total frames
    const float total_dur = dataset.total_duration();
    const double frame_dur = 1.0 / cfg_.fps;
    const int64_t total_frames = static_cast<int64_t>(std::ceil(total_dur * cfg_.fps));

    RGBAImage frame_img;
    for (int64_t f = 0; f < total_frames; ++f) {
        double time_sec = f * frame_dur;

        auto err = render_frame(dataset, time_sec, frame_img);
        if (err != VideoRenderError::Ok) {
            encoder.close();
            return err;
        }

        enc_err = encoder.write_frame(frame_img.pixels.data(), f);
        if (enc_err != VideoEncoderError::Ok) {
            encoder.close();
            return VideoRenderError::EncoderWriteFailed;
        }
    }

    auto close_err = encoder.close();
    return (close_err == VideoEncoderError::Ok)
        ? VideoRenderError::Ok
        : VideoRenderError::EncoderCloseFailed;
}

} // namespace Spectral
