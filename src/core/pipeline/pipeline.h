#pragma once

// Phase 18 — Shared generate pipeline used by CLI and Qt GUI.
// Thin API: fill GenerateConfig, call run_job(). No Qt, no CLI parsing here.

#include "error.h"
#include "project/project_config.h"
#include "spectral_dataset.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace Spectral {

// Progress: fraction 0..1, stage label ("decode", "analyze", "render", "video").
using ProgressFn = std::function<void(float, const char*)>;

// GenerateConfig is the CLI/application REQUEST: convenient, stringly,
// with UI-friendly defaults (hop 0 = fft/2, min_freq 0 = auto).
// ProjectConfig is the CANONICAL persisted configuration: validated,
// fully explicit, fingerprinted. make_project_config() is the one
// authoritative conversion; analysis and dataset metadata must come
// from its output, never from parallel ad-hoc mappings.
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
    bool output_format_explicit = false;  // true when user stated the format
                                          // (CLI --output-format / GUI combo),
                                          // false when inferred from extension
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

// JobError lives in error.h (stable codes shared with Error envelope).

// Full pipeline: decode -> analyze -> render image or video.
// Returns Ok on success. Progress may be empty (no-op).
// Cancel is polled between stages; a set flag aborts before render.
// New bytes go only to a uniquely named temp file beside the destination;
// success replaces the destination via the OS replace semantic (never
// delete-then-rename), so an interrupted or failed job never leaves a
// partial file at output_path and never destroys a previous valid output.
Error run_job(const GenerateConfig& cfg, ProgressFn progress = {},
                 const std::atomic<bool>* cancel = nullptr);

// Stages for callers needing mid-pipeline access (CLI multiband table).
// analyze_dataset fills dataset + raw mono samples; render_dataset writes output.
Error analyze_dataset(const GenerateConfig& cfg, Spectral::SpectralDataset& dataset,
                         std::vector<float>& samples_out, ProgressFn progress = {});
Error render_dataset(const GenerateConfig& cfg, const Spectral::SpectralDataset& dataset);

// Validate config without running. Returns error string, empty if valid.
std::string validate_config(const GenerateConfig& cfg);

// Canonicalize a validated request: explicit values, decode metadata,
// content identity. Callers must validate first (or handle BadConfig).
// GenerateConfig stays the CLI/application request DTO; the returned
// ProjectConfig is the persisted canonical record analysis derives from.
ProjectConfig make_project_config(const GenerateConfig& cfg,
                                  const DecodedMedia& media);

} // namespace Spectral
