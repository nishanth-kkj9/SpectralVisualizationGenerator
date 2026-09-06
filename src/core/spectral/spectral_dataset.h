#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <chrono>
#include <optional>
#include <array>

namespace Spectral {

// ============================================================================
// Schema version
// ============================================================================
constexpr uint32_t SPECTRAL_DATASET_VERSION = 2;
constexpr uint32_t SPECTRAL_DATASET_MIN_COMPATIBLE_VERSION = 1;

// ============================================================================
// Source metadata
// ============================================================================
struct SourceMetadata {
    std::string file_path;
    std::string file_hash;           // SHA256 of source file
    uint64_t file_size_bytes = 0;
    int sample_rate = 44100;
    int num_channels = 2;
    double duration_seconds = 0.0;
    std::string codec_name;
    std::string codec_long_name;
    std::chrono::system_clock::time_point analysis_timestamp;
    
    bool operator==(const SourceMetadata& other) const {
        return file_path == other.file_path &&
               file_hash == other.file_hash &&
               file_size_bytes == other.file_size_bytes &&
               sample_rate == other.sample_rate &&
               num_channels == other.num_channels &&
               duration_seconds == other.duration_seconds &&
               codec_name == other.codec_name &&
               codec_long_name == other.codec_long_name;
    }
};

// ============================================================================
// Analysis metadata
// ============================================================================
struct AnalysisMetadata {
    // Window configuration
    std::string window_type = "hann";  // "rectangular", "hann", "hamming", "blackman"
    float window_coherent_gain = 0.5f;
    
    // FFT configuration
    int fft_size = 1024;
    int hop_size = 512;
    float overlap_ratio = 0.5f;
    
    // Analysis parameters
    int sample_rate = 44100;
    int analyzed_channels = 1;           // Number of channels analyzed
    int channel_mapping = 0;             // Which channel was analyzed (0 = first/mixed)
    
    // Normalization
    float magnitude_scale = 1.0f;        // Scale applied to magnitudes
    float phase_unwrap = 0.0f;           // Phase unwrapping offset
    
    // Frequency axis
    float nyquist_frequency = 22050.0f;
    int num_frequency_bins = 513;        // N/2 + 1
    
    // Time axis
    double frame_duration_seconds = 0.0; // hop_size / sample_rate
    double total_duration_seconds = 0.0;
    int total_frames = 0;
    
    // Processing info
    std::string analyzer_version = "1.0";
    std::string analysis_method = "stft";
    int band_count = 1;
    std::chrono::system_clock::time_point analysis_timestamp;
    
    bool operator==(const AnalysisMetadata& other) const {
        return window_type == other.window_type &&
               window_coherent_gain == other.window_coherent_gain &&
               fft_size == other.fft_size &&
               hop_size == other.hop_size &&
               overlap_ratio == other.overlap_ratio &&
               sample_rate == other.sample_rate &&
               analyzed_channels == other.analyzed_channels &&
               channel_mapping == other.channel_mapping &&
               magnitude_scale == other.magnitude_scale &&
               phase_unwrap == other.phase_unwrap &&
               nyquist_frequency == other.nyquist_frequency &&
               num_frequency_bins == other.num_frequency_bins &&
               frame_duration_seconds == other.frame_duration_seconds &&
               total_duration_seconds == other.total_duration_seconds &&
               total_frames == other.total_frames &&
               analyzer_version == other.analyzer_version &&
               analysis_method == other.analysis_method &&
               band_count == other.band_count;
    }
};

// ============================================================================
// Normalization information
// ============================================================================
struct NormalizationInfo {
    // Window normalization
    float window_coherent_gain = 0.5f;   // Sum(window) / N
    float window_energy_gain = 0.0f;     // Sum(window^2) / N
    
    // Magnitude scaling
    float magnitude_scale = 1.0f;        // Overall magnitude scaling
    float reference_amplitude = 1.0f;    // Reference for dB conversion
    
    // dB conversion
    float db_floor = -90.0f;             // Minimum dB value
    float db_reference = 1.0f;           // Reference level for 0 dB
    
    // Phase
    bool phase_unwrapped = false;
    float phase_reference = 0.0f;
    
    // Channel
    bool channel_normalized = false;     // Per-channel normalization applied
    
    bool operator==(const NormalizationInfo& other) const {
        return window_coherent_gain == other.window_coherent_gain &&
               window_energy_gain == other.window_energy_gain &&
               magnitude_scale == other.magnitude_scale &&
               reference_amplitude == other.reference_amplitude &&
               db_floor == other.db_floor &&
               db_reference == other.db_reference &&
               phase_unwrapped == other.phase_unwrapped &&
               phase_reference == other.phase_reference &&
               channel_normalized == other.channel_normalized;
    }
};

// ============================================================================
// Frequency axis
// ============================================================================
struct FrequencyAxis {
    int num_bins = 0;
    int fft_size = 0;
    int sample_rate = 0;
    std::vector<float> bin_frequencies;  // Size = num_bins
    float nyquist = 0.0f;
    float resolution = 0.0f;             // Hz per bin
    
    FrequencyAxis() = default;
    FrequencyAxis(int n_fft, int sr) : fft_size(n_fft), sample_rate(sr) {
        num_bins = n_fft / 2 + 1;
        resolution = static_cast<float>(sr) / static_cast<float>(n_fft);
        nyquist = static_cast<float>(sr) / 2.0f;
        bin_frequencies.resize(num_bins);
        for (int k = 0; k < num_bins; ++k) {
            bin_frequencies[k] = k * resolution;
        }
    }
    
    bool operator==(const FrequencyAxis& other) const {
        return num_bins == other.num_bins &&
               fft_size == other.fft_size &&
               sample_rate == other.sample_rate &&
               nyquist == other.nyquist &&
               resolution == other.resolution &&
               bin_frequencies == other.bin_frequencies;
    }
};

// ============================================================================
// Time axis
// ============================================================================
struct TimeAxis {
    int num_frames = 0;
    int hop_size = 0;
    int sample_rate = 0;
    double frame_duration = 0.0;         // seconds per frame
    double total_duration = 0.0;         // seconds
    std::vector<double> frame_times;     // Size = num_frames, in seconds
    
    TimeAxis() = default;
    TimeAxis(int n_frames, int hop, int sr) : num_frames(n_frames), hop_size(hop), sample_rate(sr) {
        frame_duration = static_cast<double>(hop) / static_cast<double>(sr);
        total_duration = n_frames * frame_duration;
        frame_times.resize(n_frames);
        for (int i = 0; i < n_frames; ++i) {
            frame_times[i] = i * frame_duration;
        }
    }
    
    bool operator==(const TimeAxis& other) const {
        return num_frames == other.num_frames &&
               hop_size == other.hop_size &&
               sample_rate == other.sample_rate &&
               frame_duration == other.frame_duration &&
               total_duration == other.total_duration &&
               frame_times == other.frame_times;
    }
};

// ============================================================================
// Channel information
// ============================================================================
struct ChannelInfo {
    int total_channels = 2;
    int analyzed_channels = 1;
    int analyzed_channel_index = 0;      // 0-based index of analyzed channel
    std::vector<std::string> channel_names; // e.g., ["L", "R"]
    bool channels_mixed = false;         // True if channels were mixed to mono
    
    bool operator==(const ChannelInfo& other) const {
        return total_channels == other.total_channels &&
               analyzed_channels == other.analyzed_channels &&
               analyzed_channel_index == other.analyzed_channel_index &&
               channel_names == other.channel_names &&
               channels_mixed == other.channels_mixed;
    }
};

// ============================================================================
// Spectral frame (enhanced)
// ============================================================================
struct SpectralFrame {
    int frame_index = 0;
    int n_fft = 1024;
    float window_factor = 0.5f;
    double timestamp = 0.0;              // seconds from start
    
    // Spectral data (size = n_fft/2 + 1)
    std::vector<float> magnitudes;
    std::vector<float> phases;
    std::vector<float> power;            // magnitude^2
    
    // Per-frame metadata
    float rms = 0.0f;
    float peak_magnitude = 0.0f;
    float spectral_centroid = 0.0f;
    float spectral_bandwidth = 0.0f;
    
    // Reassigned coordinates (empty for conventional STFT)
    std::vector<float> reassigned_times;   // group delay (seconds)
    std::vector<float> reassigned_freqs;   // instantaneous frequency (Hz)
    int band_count = 1;
    
    bool operator==(const SpectralFrame& other) const {
        return frame_index == other.frame_index &&
               n_fft == other.n_fft &&
               window_factor == other.window_factor &&
               timestamp == other.timestamp &&
               magnitudes == other.magnitudes &&
               phases == other.phases &&
               power == other.power &&
               rms == other.rms &&
               peak_magnitude == other.peak_magnitude &&
               spectral_centroid == other.spectral_centroid &&
               spectral_bandwidth == other.spectral_bandwidth &&
               reassigned_times == other.reassigned_times &&
               reassigned_freqs == other.reassigned_freqs &&
               band_count == other.band_count;
    }
};

// ============================================================================
// Validation result
// ============================================================================
struct ValidationResult {
    bool valid = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    
    void add_error(const std::string& msg) {
        errors.push_back(msg);
        valid = false;
    }
    
    void add_warning(const std::string& msg) {
        warnings.push_back(msg);
    }
    
    bool has_errors() const { return !errors.empty(); }
    bool has_warnings() const { return !warnings.empty(); }
};

// ============================================================================
// Serialization format
// ============================================================================
enum class SerializationFormat {
    Binary = 0,      // Deterministic binary format
    JSON = 1,        // Human-readable JSON
};

// ============================================================================
// SpectralDataset - Complete architecture
// ============================================================================
class SpectralDataset {
public:
    // Constructors
    SpectralDataset() = default;
    ~SpectralDataset() = default;
    
    // ========================================================================
    // Validation
    // ========================================================================
    ValidationResult validate() const;
    
    // Dimension consistency checks
    bool check_dimensions() const;
    bool check_metadata_consistency() const;
    bool check_data_integrity() const;
    
    // ========================================================================
    // Serialization
    // ========================================================================
    // Binary serialization (deterministic)
    bool serialize_binary(std::vector<uint8_t>& out) const;
    bool deserialize_binary(const uint8_t* data, size_t size);
    
    // JSON serialization (human-readable)
    std::string serialize_json(bool pretty = true) const;
    bool deserialize_json(const std::string& json);
    
    // File I/O
    bool save_to_file(const std::string& path, SerializationFormat fmt = SerializationFormat::Binary) const;
    bool load_from_file(const std::string& path, SerializationFormat fmt = SerializationFormat::Binary);
    
    // ========================================================================
    // Data access
    // ========================================================================
    // Frames
    void add_frame(const SpectralFrame& frame);
    bool get_frame(int index, SpectralFrame& out) const;
    const SpectralFrame& frame(int index) const;
    int frame_count() const { return static_cast<int>(frames_.size()); }
    
    // Axes
    const FrequencyAxis& frequency_axis() const { return freq_axis_; }
    const TimeAxis& time_axis() const { return time_axis_; }
    const ChannelInfo& channel_info() const { return channel_info_; }
    
    // Metadata
    const SourceMetadata& source_metadata() const { return source_meta_; }
    const AnalysisMetadata& analysis_metadata() const { return analysis_meta_; }
    const NormalizationInfo& normalization_info() const { return normalization_; }
    
    // Mutable access for building
    SourceMetadata& mutable_source_metadata() { return source_meta_; }
    AnalysisMetadata& mutable_analysis_metadata() { return analysis_meta_; }
    NormalizationInfo& mutable_normalization_info() { return normalization_; }
    FrequencyAxis& mutable_frequency_axis() { return freq_axis_; }
    TimeAxis& mutable_time_axis() { return time_axis_; }
    ChannelInfo& mutable_channel_info() { return channel_info_; }
    
    // ========================================================================
    // Computed properties
    // ========================================================================
    double total_duration() const { return time_axis_.total_duration; }
    double frame_duration() const { return time_axis_.frame_duration; }
    int sample_rate() const { return analysis_meta_.sample_rate; }
    int fft_size() const { return analysis_meta_.fft_size; }
    int hop_size() const { return analysis_meta_.hop_size; }
    int num_frequency_bins() const { return freq_axis_.num_bins; }
    float nyquist_frequency() const { return freq_axis_.nyquist; }
    float frequency_resolution() const { return freq_axis_.resolution; }
    
    // Spectral statistics
    std::vector<float> mean_magnitude_spectrum() const;
    std::vector<float> max_magnitude_spectrum() const;
    std::vector<float> min_magnitude_spectrum() const;
    
    // ========================================================================
    // Filtering/transforms
    // ========================================================================
    SpectralDataset filter_band(float low_hz, float high_hz) const;
    SpectralDataset filter_time(double start_sec, double end_sec) const;
    SpectralDataset downsample_time(int factor) const;
    SpectralDataset downsample_frequency(int factor) const;
    
    // Export
    bool export_csv(const std::string& path) const;
    
    // Version
    static uint32_t current_version() { return SPECTRAL_DATASET_VERSION; }
    static uint32_t min_compatible_version() { return SPECTRAL_DATASET_MIN_COMPATIBLE_VERSION; }
    uint32_t version() const { return version_; }
    
    // Comparison
    bool operator==(const SpectralDataset& other) const;

private:
    uint32_t version_ = SPECTRAL_DATASET_VERSION;
    
    // Core data
    std::vector<SpectralFrame> frames_;
    
    // Metadata
    SourceMetadata source_meta_;
    AnalysisMetadata analysis_meta_;
    NormalizationInfo normalization_;
    FrequencyAxis freq_axis_;
    TimeAxis time_axis_;
    ChannelInfo channel_info_;
    
    // Internal validation
    void validate_dimensions(ValidationResult& result) const;
    void validate_metadata(ValidationResult& result) const;
    void validate_data(ValidationResult& result) const;
    
    // Serialization helpers
    void write_header(std::vector<uint8_t>& out) const;
    bool read_header(const uint8_t* data, size_t size, size_t& offset);
    void write_frames(std::vector<uint8_t>& out) const;
    bool read_frames(const uint8_t* data, size_t size, size_t& offset);
    void write_metadata(std::vector<uint8_t>& out) const;
    bool read_metadata(const uint8_t* data, size_t size, size_t& offset);
    void write_axes(std::vector<uint8_t>& out) const;
    bool read_axes(const uint8_t* data, size_t size, size_t& offset);
};

} // namespace Spectral