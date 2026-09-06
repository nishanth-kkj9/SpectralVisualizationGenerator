#pragma once

// Phase 8 — Project / configuration format for reproducible processing.
// A single JSON document records every input that affects analysis or
// rendering output. The same ProjectConfig loaded twice must produce the
// same analytical configuration (round-trip equality) and the same input
// -> same bytes for the deterministic tiers (Spectral, Image).
//
// Video tier is explicitly NOT byte-identical; the config records which
// encoder/parameters were used so a re-encode can reproduce a *similar*
// file, but lossless byte equality is not promised.

#include <cstdint>
#include <string>
#include <vector>

namespace Spectral {

// ============================================================================
// Software / schema versions (bump on incompatible changes)
// ============================================================================
constexpr uint32_t PROJECT_CONFIG_SCHEMA_VERSION = 1;
constexpr uint32_t PROJECT_CONFIG_MIN_COMPATIBLE_VERSION = 1;
constexpr const char* PROJECT_CONFIG_SOFTWARE_NAME = "SpectralVisualizationGenerator";
constexpr const char* PROJECT_CONFIG_SOFTWARE_VERSION = "0.1.0";

// ============================================================================
// Reproducibility tier — describes what the config can guarantee.
// ============================================================================
enum class ReproducibilityTier {
    // Same ProjectConfig + same input samples -> bit-identical spectral data.
    // Same ProjectConfig + same SpectralDataset -> bit-identical image.
    // Video: NOT byte-identical (lossy codecs may vary between runs).
    Spectral_Image_NotVideo = 0,
    // No guarantees beyond round-trip equality of the config itself.
    None = 1,
};

// ============================================================================
// Renderer kind — which renderer this config drives
// ============================================================================
enum class RendererKind {
    Spectrum = 0,
    Spectrogram = 1,
};

// ============================================================================
// Frequency axis scale (shared with renderer configs)
// ============================================================================
enum class ProjectFreqScale {
    Linear = 0,
    Logarithmic = 1,
};

// ============================================================================
// Color map (matches renderer's enum)
// ============================================================================
enum class ProjectColorMap {
    Viridis = 0,
    Heat = 1,
};

// ============================================================================
// Window type (matches the analyzer)
// ============================================================================
enum class ProjectWindowType {
    Rectangular = 0,
    Hann = 1,
    Hamming = 2,
    Blackman = 3,
};

// ============================================================================
// Input source description
// ============================================================================
struct ProjectInput {
    std::string file_path;          // empty if non-file
    std::string file_hash;          // SHA256 hex
    uint64_t file_size_bytes = 0;
    int sample_rate = 44100;        // canonicalized post-decode
    int num_channels = 1;           // 1 = mono (downmixed at decode time)
    double duration_seconds = 0.0;  // canonicalized
    std::string codec_name;         // informational
    std::string codec_long_name;    // informational

    bool operator==(const ProjectInput& o) const {
        return file_path == o.file_path &&
               file_hash == o.file_hash &&
               file_size_bytes == o.file_size_bytes &&
               sample_rate == o.sample_rate &&
               num_channels == o.num_channels &&
               duration_seconds == o.duration_seconds &&
               codec_name == o.codec_name &&
               codec_long_name == o.codec_long_name;
    }
};

// ============================================================================
// Analysis parameters (drives SpectralDataset::AnalysisMetadata)
// ============================================================================
struct ProjectAnalysis {
    int fft_size = 1024;
    int hop_size = 512;
    float overlap_ratio = 0.5f;
    ProjectWindowType window_type = ProjectWindowType::Hann;
    float window_coherent_gain = 0.5f;  // sum(window)/N for the canonical window
    int sample_rate = 44100;
    int analyzed_channels = 1;          // always 1: pre-mixed if needed
    int channel_mapping = 0;            // 0-based source channel index
    float magnitude_scale = 1.0f;       // post-FFT linear scale
    float phase_unwrap = 0.0f;          // 0 = no unwrap (informational)

    bool operator==(const ProjectAnalysis& o) const {
        return fft_size == o.fft_size &&
               hop_size == o.hop_size &&
               overlap_ratio == o.overlap_ratio &&
               window_type == o.window_type &&
               window_coherent_gain == o.window_coherent_gain &&
               sample_rate == o.sample_rate &&
               analyzed_channels == o.analyzed_channels &&
               channel_mapping == o.channel_mapping &&
               magnitude_scale == o.magnitude_scale &&
               phase_unwrap == o.phase_unwrap;
    }
};

// ============================================================================
// Normalization / dynamic range
// ============================================================================
struct ProjectDynamicRange {
    float db_floor = -90.0f;
    float db_ceiling = 0.0f;
    float reference_amplitude = 1.0f;
    float window_energy_gain = 0.0f;   // sum(window^2)/N

    bool operator==(const ProjectDynamicRange& o) const {
        return db_floor == o.db_floor &&
               db_ceiling == o.db_ceiling &&
               reference_amplitude == o.reference_amplitude &&
               window_energy_gain == o.window_energy_gain;
    }
};

// ============================================================================
// Frequency display range
// ============================================================================
struct ProjectFrequencyRange {
    float min_hz = 20.0f;     // 0 = auto (DC)
    float max_hz = 0.0f;      // 0 = auto (nyquist)
    ProjectFreqScale scale = ProjectFreqScale::Logarithmic;

    bool operator==(const ProjectFrequencyRange& o) const {
        return min_hz == o.min_hz &&
               max_hz == o.max_hz &&
               scale == o.scale;
    }
};

// ============================================================================
// Renderer (image) parameters
// ============================================================================
struct ProjectRenderer {
    RendererKind kind = RendererKind::Spectrum;
    ProjectColorMap color_map = ProjectColorMap::Viridis;
    int width = 1024;
    int height = 512;
    int dpi = 96;                          // informational
    bool draw_grid = true;
    bool draw_labels = true;
    int line_thickness = 2;                // spectrum only
    int grid_divisions_x = 8;
    int grid_divisions_y = 6;

    bool operator==(const ProjectRenderer& o) const {
        return kind == o.kind &&
               color_map == o.color_map &&
               width == o.width &&
               height == o.height &&
               dpi == o.dpi &&
               draw_grid == o.draw_grid &&
               draw_labels == o.draw_labels &&
               line_thickness == o.line_thickness &&
               grid_divisions_x == o.grid_divisions_x &&
               grid_divisions_y == o.grid_divisions_y;
    }
};

// ============================================================================
// Project configuration — the top-level document
// ============================================================================
struct ProjectConfig {
    ProjectInput input;
    ProjectAnalysis analysis;
    ProjectDynamicRange dynamic_range;
    ProjectFrequencyRange frequency_range;
    ProjectRenderer renderer;

    // Software / version tracking
    uint32_t schema_version = PROJECT_CONFIG_SCHEMA_VERSION;
    std::string software_name = PROJECT_CONFIG_SOFTWARE_NAME;
    std::string software_version = PROJECT_CONFIG_SOFTWARE_VERSION;
    std::string project_id;                 // optional user-supplied id
    std::string created_utc;                // ISO8601 string (informational)
    std::string notes;                      // optional user-supplied

    ReproducibilityTier reproducibility_tier = ReproducibilityTier::Spectral_Image_NotVideo;

    bool operator==(const ProjectConfig& o) const {
        return input == o.input &&
               analysis == o.analysis &&
               dynamic_range == o.dynamic_range &&
               frequency_range == o.frequency_range &&
               renderer == o.renderer &&
               schema_version == o.schema_version &&
               software_name == o.software_name &&
               software_version == o.software_version &&
               project_id == o.project_id &&
               created_utc == o.created_utc &&
               notes == o.notes &&
               reproducibility_tier == o.reproducibility_tier;
    }
    bool operator!=(const ProjectConfig& o) const { return !(*this == o); }

    // Validate internal consistency. Returns true if all checks pass.
    // On failure, populates `errors`.
    bool validate(std::vector<std::string>& errors) const;

    // Convenience
    std::string fingerprint() const;        // SHA256 of canonical JSON (no comments/whitespace)
};

// ============================================================================
// Serialization (canonical JSON, deterministic key order)
// ============================================================================
class ProjectConfigSerializer {
public:
    // Serialize to canonical JSON (sorted keys, no indentation).
    static std::string to_json(const ProjectConfig& cfg);

    // Pretty-printed JSON for human inspection (sorted keys).
    static std::string to_json_pretty(const ProjectConfig& cfg, int indent = 2);

    // Parse JSON into a ProjectConfig. Returns true on success.
    // On failure, populates `error`. The parse is strict: unknown fields are
    // rejected (forward-compatibility guard) unless `allow_unknown` is true.
    static bool from_json(const std::string& json, ProjectConfig& out,
                          std::string& error,
                          bool allow_unknown = false);

    // File I/O
    static bool save(const std::string& path, const ProjectConfig& cfg,
                     std::string& error, bool pretty = false);
    static bool load(const std::string& path, ProjectConfig& out,
                     std::string& error, bool allow_unknown = false);
};

// ============================================================================
// Adapter — translate a ProjectConfig into the configs the existing
// analysis and renderer code consumes (no duplication of fields).
// ============================================================================
class ProjectConfigAdapter {
public:
    // Build a SpectralDataset::AnalysisMetadata from a ProjectConfig.
    // Caller fills in source_metadata separately (file hash, decode info, etc.).
    static void to_analysis_metadata(const ProjectConfig& cfg,
                                      class AnalysisMetadata& out);

    // Build a SpectrumConfig from a ProjectConfig.
    static void to_spectrum_config(const ProjectConfig& cfg,
                                   class SpectrumConfig& out);

    // Build a SpectrogramConfig from a ProjectConfig.
    static void to_spectrogram_config(const ProjectConfig& cfg,
                                      class SpectrogramConfig& out);
};

// ============================================================================
// Helpers
// ============================================================================
// Convert between enums used in the ProjectConfig and the renderer's enums.
ProjectFreqScale to_renderer_freq_scale(ProjectFreqScale s);  // identity
ProjectColorMap to_renderer_color_map(ProjectColorMap m);    // identity

// Window-type helpers (string <-> enum).
const char* window_type_name(ProjectWindowType w);
ProjectWindowType parse_window_type(const std::string& s);

const char* renderer_kind_name(RendererKind k);
RendererKind parse_renderer_kind(const std::string& s);

const char* freq_scale_name(ProjectFreqScale s);
ProjectFreqScale parse_freq_scale(const std::string& s);

const char* color_map_name(ProjectColorMap m);
ProjectColorMap parse_color_map(const std::string& s);

const char* reproducibility_tier_name(ReproducibilityTier t);

} // namespace Spectral
