#pragma once

// Phase 18 — Shared generate pipeline used by CLI and Qt GUI.
// Thin API: fill GenerateConfig, call run_job(). No Qt, no CLI parsing here.

#include "spectral_dataset.h"

#include <functional>
#include <string>
#include <vector>

namespace Spectral {

// Progress: fraction 0..1, stage label ("decode", "analyze", "render", "video").
using ProgressFn = std::function<void(float, const char*)>;

struct GenerateConfig {
    std::string input_path;
    std::string output_path;
    std::string visualization = "spectrogram";  // spectrogram | spectrum
    int fft_size = 1024;
    int hop_size = 0;       // 0 = fft_size/2
    std::string window = "hann";
    float overlap = 0.5f;
    float min_freq = 0.0f;
    float max_freq = 0.0f;  // 0 = auto (nyquist)
    float db_range = 80.0f;
    int width = 1024;
    int height = 512;
    std::string output_format = "image";  // image | video
    int fps = 30;
    std::string video_codec = "libx264";
    int crf = 18;
    float window_seconds = 5.0f;
    std::string freq_scale = "log";
    float cqt_center = 440.0f;
    float cqt_q = 12.0f;
    bool reassigned = false;
    bool use_gpu = false;  // spectrogram render_gpu() with CPU fallback
};

enum class JobError {
    Ok = 0,
    FileNotFound,
    DecodeError,
    AnalysisError,
    RenderError,
    BadConfig,
};

// Full pipeline: decode -> analyze -> render image or video.
// Returns Ok on success. Progress may be empty (no-op).
JobError run_job(const GenerateConfig& cfg, ProgressFn progress = {});

// Stages for callers needing mid-pipeline access (CLI multiband table).
// analyze_dataset fills dataset + raw mono samples; render_dataset writes output.
JobError analyze_dataset(const GenerateConfig& cfg, Spectral::SpectralDataset& dataset,
                         std::vector<float>& samples_out, ProgressFn progress = {});
JobError render_dataset(const GenerateConfig& cfg, const Spectral::SpectralDataset& dataset);

// Validate config without running. Returns error string, empty if valid.
std::string validate_config(const GenerateConfig& cfg);

} // namespace Spectral
