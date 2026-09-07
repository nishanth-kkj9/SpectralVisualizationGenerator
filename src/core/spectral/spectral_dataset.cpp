#include "spectral_dataset.h"

#include "project/project_config.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>

namespace Spectral {

// ============================================================================
// Serialization format constants
// ============================================================================
namespace {

constexpr uint32_t BINARY_MAGIC = 0x53504454;  // "SPDT" (Spectral Dataset)
constexpr uint32_t BINARY_HEADER_SIZE = 64;   // Fixed header size
constexpr size_t MAX_DATASET_SIZE = 1024ull * 1024ull * 1024ull * 4ull; // 4 GB hard cap
// Per-field hard bounds (documented; see docs/reproducibility.md).
constexpr uint64_t MAX_STR_BYTES = 4ull << 20;  // 4 MB per string
constexpr uint64_t MAX_FRAMES = 1ull << 24;     // 16M frames
constexpr uint64_t MAX_BINS = 1ull << 20;       // 1M bins
constexpr uint64_t MAX_NAMES = 256;             // channel names

// FNV-1a 64-bit header checksum: corruption detection ONLY, not identity
// (identity is SHA-256 over canonical bytes; see dataset_identity).
constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;

uint64_t fnv1a_64(const uint8_t* data, size_t len) {
    uint64_t h = FNV_OFFSET;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= FNV_PRIME;
    }
    return h;
}

// v3 explicit little-endian wire codec. No native struct memcpy: byte
// order and padding are platform properties, never the format contract.
inline void w_u8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
inline void w_u32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v >> 16));
    o.push_back(static_cast<uint8_t>(v >> 24));
}
inline void w_i32(std::vector<uint8_t>& o, int32_t v) {
    w_u32(o, static_cast<uint32_t>(v));
}
inline void w_u64(std::vector<uint8_t>& o, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void w_f32(std::vector<uint8_t>& o, float v) {
    uint32_t u = 0;
    static_assert(sizeof(float) == 4, "IEEE-754 float32 required");
    std::memcpy(&u, &v, 4);
    w_u32(o, u);
}
inline void w_f64(std::vector<uint8_t>& o, double v) {
    uint64_t u = 0;
    static_assert(sizeof(double) == 8, "IEEE-754 float64 required");
    std::memcpy(&u, &v, 8);
    w_u64(o, u);
}
inline void w_bool(std::vector<uint8_t>& o, bool v) { o.push_back(v ? 1 : 0); }
inline void w_str(std::vector<uint8_t>& o, const std::string& s) {
    w_u32(o, static_cast<uint32_t>(s.size()));
    o.insert(o.end(), s.begin(), s.end());
}

inline bool need_bytes(size_t size, size_t off, size_t n) {
    return n <= size && off <= size - n;
}
inline bool r_u8(const uint8_t* data, size_t size, size_t& off, uint8_t& v) {
    if (!need_bytes(size, off, 1)) return false;
    v = data[off++];
    return true;
}
inline bool r_u32(const uint8_t* data, size_t size, size_t& off, uint32_t& v) {
    if (!need_bytes(size, off, 4)) return false;
    v = static_cast<uint32_t>(data[off]) |
        (static_cast<uint32_t>(data[off + 1]) << 8) |
        (static_cast<uint32_t>(data[off + 2]) << 16) |
        (static_cast<uint32_t>(data[off + 3]) << 24);
    off += 4;
    return true;
}
inline bool r_i32(const uint8_t* data, size_t size, size_t& off, int32_t& v) {
    uint32_t u = 0;
    if (!r_u32(data, size, off, u)) return false;
    v = static_cast<int32_t>(u);
    return true;
}
inline bool r_u64(const uint8_t* data, size_t size, size_t& off, uint64_t& v) {
    if (!need_bytes(size, off, 8)) return false;
    v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(data[off + i]) << (8 * i);
    off += 8;
    return true;
}
inline bool r_f32(const uint8_t* data, size_t size, size_t& off, float& v) {
    uint32_t u = 0;
    if (!r_u32(data, size, off, u)) return false;
    static_assert(sizeof(float) == 4, "IEEE-754 float32 required");
    std::memcpy(&v, &u, 4);
    return true;
}
inline bool r_f64(const uint8_t* data, size_t size, size_t& off, double& v) {
    uint64_t u = 0;
    if (!r_u64(data, size, off, u)) return false;
    static_assert(sizeof(double) == 8, "IEEE-754 float64 required");
    std::memcpy(&v, &u, 8);
    return true;
}
inline bool r_bool(const uint8_t* data, size_t size, size_t& off, bool& v) {
    uint8_t b = 0;
    if (!r_u8(data, size, off, b)) return false;
    if (b > 1) return false;
    v = (b == 1);
    return true;
}
inline bool r_str(const uint8_t* data, size_t size, size_t& off, std::string& s) {
    uint32_t n = 0;
    if (!r_u32(data, size, off, n)) return false;
    if (n > MAX_STR_BYTES || n > size - off) return false;
    s.assign(reinterpret_cast<const char*>(data + off), n);
    off += n;
    return true;
}
// Bounded array read: count cap + exact byte precheck before any
// allocation, so hostile lengths fail before memory moves.
inline bool read_f32_array(const uint8_t* data, size_t size, size_t& off,
                           uint64_t count, uint64_t cap, std::vector<float>& out) {
    if (count > cap) return false;
    if (count > (MAX_DATASET_SIZE - off) / 4) return false;
    if (off + count * 4 > size) return false;
    out.resize(static_cast<size_t>(count));
    for (uint64_t k = 0; k < count; ++k) {
        if (!r_f32(data, size, off, out[static_cast<size_t>(k)])) return false;
    }
    return true;
}

bool is_power_of_two(int n) {
    return n > 0 && (n & (n - 1)) == 0;
}

// Tiny JSON escaper (handles only what we emit)
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string fp_to_string(float v) {
    // Deterministic: fixed precision, no locale
    std::ostringstream os;
    os.setf(std::ios::scientific);
    os << std::setprecision(9) << v;
    return os.str();
}

std::string fp_to_string(double v) {
    std::ostringstream os;
    os.setf(std::ios::scientific);
    os << std::setprecision(17) << v;
    return os.str();
}

// Parse a JSON numeric literal (scientific or decimal). Robust enough for
// what we emit. Returns NaN on failure (caller checks).
double parse_json_number(const std::string& s) {
    if (s.empty()) return std::numeric_limits<double>::quiet_NaN();
    try {
        size_t idx = 0;
        double v = std::stod(s, &idx);
        if (idx != s.size()) return std::numeric_limits<double>::quiet_NaN();
        return v;
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

// Minimal JSON string -> std::string
std::string json_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            switch (n) {
                case '"':  out += '"';  i++; break;
                case '\\': out += '\\'; i++; break;
                case '/':  out += '/';  i++; break;
                case 'n':  out += '\n'; i++; break;
                case 'r':  out += '\r'; i++; break;
                case 't':  out += '\t'; i++; break;
                case 'b':  out += '\b'; i++; break;
                case 'f':  out += '\f'; i++; break;
                default:   out += s[i];
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

// ============================================================================
// Validation
// ============================================================================

void SpectralDataset::validate_dimensions(ValidationResult& r) const {
    const int nb = freq_axis_.num_bins;
    if (nb <= 0) {
        r.add_error("frequency_axis.num_bins must be > 0");
        return;
    }
    if (nb != analysis_meta_.num_frequency_bins) {
        r.add_error("frequency_axis.num_bins (" + std::to_string(nb) +
                    ") != analysis_metadata.num_frequency_bins (" +
                    std::to_string(analysis_meta_.num_frequency_bins) + ")");
    }
    if (nb != freq_axis_.bin_frequencies.size()) {
        r.add_error("frequency_axis.bin_frequencies size mismatch");
    }
    if (analysis_meta_.fft_size > 0) {
        const int expected = analysis_meta_.fft_size / 2 + 1;
        if (nb != expected) {
            r.add_error("num_bins (" + std::to_string(nb) +
                        ") != fft_size/2+1 (" + std::to_string(expected) + ")");
        }
    }
    if (time_axis_.num_frames != static_cast<int>(frames_.size())) {
        r.add_error("time_axis.num_frames (" + std::to_string(time_axis_.num_frames) +
                    ") != frames_.size() (" + std::to_string(frames_.size()) + ")");
    }
    if (time_axis_.frame_times.size() != frames_.size()) {
        r.add_error("time_axis.frame_times size mismatch");
    }
    // Expected frame width follows the representation: FFT-grid bins for
    // STFT, explicit representation bins otherwise (never N/2+1 there).
    const int expect_bins = (!representation_.is_stft() && representation_.bins > 0)
                                ? representation_.bins
                                : nb;
    for (size_t i = 0; i < frames_.size(); ++i) {
        const auto& f = frames_[i];
        if (static_cast<int>(f.magnitudes.size()) != expect_bins) {
            r.add_error("frame[" + std::to_string(i) +
                        "].magnitudes size != expected bins");
        }
        if (static_cast<int>(f.phases.size()) != expect_bins) {
            r.add_error("frame[" + std::to_string(i) +
                        "].phases size != expected bins");
        }
        if (static_cast<int>(f.power.size()) != expect_bins) {
            r.add_error("frame[" + std::to_string(i) +
                        "].power size != expected bins");
        }
        // Reassigned coordinates are either absent (conventional STFT)
        // or cover every bin.
        if (!f.reassigned_times.empty() &&
            static_cast<int>(f.reassigned_times.size()) != expect_bins) {
            r.add_error("frame[" + std::to_string(i) +
                        "].reassigned_times size != expected bins");
        }
        if (!f.reassigned_freqs.empty() &&
            static_cast<int>(f.reassigned_freqs.size()) != expect_bins) {
            r.add_error("frame[" + std::to_string(i) +
                        "].reassigned_freqs size != expected bins");
        }
        // n_fft is an STFT concept; non-STFT frames carry no FFT size.
        if (representation_.is_stft() && f.n_fft != analysis_meta_.fft_size) {
            r.add_error("frame[" + std::to_string(i) +
                        "].n_fft (" + std::to_string(f.n_fft) +
                        ") != analysis.fft_size (" +
                        std::to_string(analysis_meta_.fft_size) + ")");
        }
        // Timestamp convention: frame start == index * hop / rate.
        if (analysis_meta_.sample_rate > 0 && analysis_meta_.hop_size > 0) {
            const double expect = static_cast<double>(f.frame_index) *
                                  analysis_meta_.hop_size / analysis_meta_.sample_rate;
            const double tol = 1e-9 * (std::fabs(expect) > 1.0 ? std::fabs(expect) : 1.0);
            if (std::fabs(f.timestamp - expect) > tol) {
                r.add_error("frame[" + std::to_string(i) + "].timestamp != index*hop/rate");
            }
        }
        if (!std::isfinite(f.timestamp)) {
            r.add_error("frame[" + std::to_string(i) + "].timestamp not finite");
        }
    }
    // Bin frequencies must follow k * sr / N, nonnegative and ordered.
    if (analysis_meta_.fft_size > 0 && analysis_meta_.sample_rate > 0) {
        for (int k = 0; k < nb && k < static_cast<int>(freq_axis_.bin_frequencies.size()); ++k) {
            const float expect = static_cast<float>(k) * analysis_meta_.sample_rate /
                                 static_cast<float>(analysis_meta_.fft_size);
            if (std::fabs(freq_axis_.bin_frequencies[k] - expect) > 1e-3f * (expect + 1.0f)) {
                r.add_error("frequency_axis.bin_frequencies[" + std::to_string(k) +
                            "] != k*sr/N");
                break;
            }
            if (freq_axis_.bin_frequencies[k] < 0.0f) {
                r.add_error("frequency_axis.bin_frequencies has negative entry");
                break;
            }
            if (k > 0 && freq_axis_.bin_frequencies[k] < freq_axis_.bin_frequencies[k - 1]) {
                r.add_error("frequency_axis.bin_frequencies not ordered");
                break;
            }
        }
    }
    if (!std::isfinite(freq_axis_.nyquist) || !std::isfinite(freq_axis_.resolution)) {
        r.add_error("frequency_axis nyquist/resolution not finite");
    }
    if (!std::isfinite(time_axis_.frame_duration) || !std::isfinite(time_axis_.total_duration)) {
        r.add_error("time_axis durations not finite");
    }
    // Representation coherence: WHAT the bins mean must agree with their shape.
    {
        std::vector<std::string> rep_errs;
        validate_representation(representation_, analysis_meta_.fft_size, rep_errs);
        for (const auto& e : rep_errs) r.add_error("representation: " + e);
    }
    // Reassigned coordinates without representation support are meaningless.
    if (!representation_.reassignment_supported) {
        for (size_t i = 0; i < frames_.size(); ++i) {
            if (!frames_[i].reassigned_times.empty() || !frames_[i].reassigned_freqs.empty()) {
                r.add_error("frame[" + std::to_string(i) +
                            "]: reassigned data without representation support");
                break;
            }
        }
    }
}

void SpectralDataset::validate_metadata(ValidationResult& r) const {
    if (analysis_meta_.fft_size <= 0) {
        r.add_error("analysis_metadata.fft_size must be > 0");
    } else if (!is_power_of_two(analysis_meta_.fft_size)) {
        r.add_error("analysis_metadata.fft_size must be a power of 2");
    }
    if (analysis_meta_.hop_size <= 0) {
        r.add_error("analysis_metadata.hop_size must be > 0");
    }
    if (analysis_meta_.hop_size > analysis_meta_.fft_size) {
        r.add_warning("hop_size > fft_size (frames will not overlap)");
    }
    if (analysis_meta_.sample_rate <= 0) {
        r.add_error("analysis_metadata.sample_rate must be > 0");
    }
    if (analysis_meta_.analyzed_channels <= 0) {
        r.add_error("analyzed_channels must be > 0");
    }
    if (analysis_meta_.analyzed_channels > channel_info_.total_channels &&
        channel_info_.total_channels > 0) {
        r.add_warning("analyzed_channels > total_channels");
    }
    if (channel_info_.analyzed_channel_index < 0 ||
        channel_info_.analyzed_channel_index >= channel_info_.total_channels) {
        if (channel_info_.total_channels > 0) {
            r.add_warning("analyzed_channel_index out of range");
        }
    }
    if (freq_axis_.sample_rate != analysis_meta_.sample_rate) {
        r.add_error("frequency_axis.sample_rate != analysis.sample_rate");
    }
    if (time_axis_.sample_rate != analysis_meta_.sample_rate) {
        r.add_error("time_axis.sample_rate != analysis.sample_rate");
    }
    if (analysis_meta_.nyquist_frequency != freq_axis_.nyquist) {
        r.add_error("analysis_metadata.nyquist != frequency_axis.nyquist");
    }
    if (time_axis_.hop_size != analysis_meta_.hop_size) {
        r.add_error("time_axis.hop_size != analysis.hop_size");
    }
}

void SpectralDataset::validate_data(ValidationResult& r) const {
    for (size_t i = 0; i < frames_.size(); ++i) {
        const auto& f = frames_[i];
        for (size_t k = 0; k < f.magnitudes.size(); ++k) {
            if (!std::isfinite(f.magnitudes[k])) {
                r.add_error("frame[" + std::to_string(i) +
                            "].magnitudes[" + std::to_string(k) +
                            "] is not finite");
                return;
            }
        }
        for (size_t k = 0; k < f.phases.size(); ++k) {
            if (!std::isfinite(f.phases[k])) {
                r.add_error("frame[" + std::to_string(i) +
                            "].phases[" + std::to_string(k) +
                            "] is not finite");
                return;
            }
        }
    }
}

ValidationResult SpectralDataset::validate() const {
    ValidationResult r;
    r.valid = true;
    validate_dimensions(r);
    validate_metadata(r);
    validate_data(r);
    return r;
}

bool SpectralDataset::check_dimensions() const {
    ValidationResult r;
    r.valid = true;
    validate_dimensions(r);
    return !r.has_errors();
}

bool SpectralDataset::check_metadata_consistency() const {
    ValidationResult r;
    r.valid = true;
    validate_metadata(r);
    return !r.has_errors();
}

bool SpectralDataset::check_data_integrity() const {
    ValidationResult r;
    r.valid = true;
    validate_data(r);
    return !r.has_errors();
}

// ============================================================================
// Data access
// ============================================================================

void SpectralDataset::add_frame(const SpectralFrame& frame) {
    frames_.push_back(frame);
    time_axis_.num_frames = static_cast<int>(frames_.size());
    time_axis_.frame_duration =
        static_cast<double>(time_axis_.hop_size) /
        static_cast<double>(time_axis_.sample_rate);
    time_axis_.total_duration =
        time_axis_.num_frames * time_axis_.frame_duration;
    time_axis_.frame_times.resize(frames_.size());
    for (int i = 0; i < time_axis_.num_frames; ++i) {
        time_axis_.frame_times[i] = i * time_axis_.frame_duration;
    }
    if (frame.n_fft > 0) {
        time_axis_.hop_size = (analysis_meta_.hop_size > 0) ? analysis_meta_.hop_size : time_axis_.hop_size;
    }
    analysis_meta_.total_frames = time_axis_.num_frames;
    analysis_meta_.total_duration_seconds = time_axis_.total_duration;
    analysis_meta_.frame_duration_seconds = time_axis_.frame_duration;
}

bool SpectralDataset::get_frame(int index, SpectralFrame& out) const {
    if (index < 0 || index >= static_cast<int>(frames_.size())) return false;
    out = frames_[index];
    return true;
}

const SpectralFrame& SpectralDataset::frame(int index) const {
    return frames_.at(index);
}

// ============================================================================
// Statistics
// ============================================================================

std::vector<float> SpectralDataset::mean_magnitude_spectrum() const {
    if (frames_.empty() || freq_axis_.num_bins <= 0) return {};
    std::vector<float> out(freq_axis_.num_bins, 0.0f);
    for (const auto& f : frames_) {
        for (int k = 0; k < freq_axis_.num_bins; ++k) {
            out[k] += f.magnitudes[k];
        }
    }
    const float inv = 1.0f / static_cast<float>(frames_.size());
    for (auto& v : out) v *= inv;
    return out;
}

std::vector<float> SpectralDataset::max_magnitude_spectrum() const {
    if (frames_.empty() || freq_axis_.num_bins <= 0) return {};
    std::vector<float> out(freq_axis_.num_bins,
                          -std::numeric_limits<float>::infinity());
    for (const auto& f : frames_) {
        for (int k = 0; k < freq_axis_.num_bins; ++k) {
            out[k] = std::max(out[k], f.magnitudes[k]);
        }
    }
    return out;
}

std::vector<float> SpectralDataset::min_magnitude_spectrum() const {
    if (frames_.empty() || freq_axis_.num_bins <= 0) return {};
    std::vector<float> out(freq_axis_.num_bins,
                          std::numeric_limits<float>::infinity());
    for (const auto& f : frames_) {
        for (int k = 0; k < freq_axis_.num_bins; ++k) {
            out[k] = std::min(out[k], f.magnitudes[k]);
        }
    }
    return out;
}

// ============================================================================
// Filters
// ============================================================================

SpectralDataset SpectralDataset::filter_band(float low_hz, float high_hz) const {
    SpectralDataset out = *this;
    int lo = -1, hi = -1;
    for (int k = 0; k < freq_axis_.num_bins; ++k) {
        if (lo < 0 && freq_axis_.bin_frequencies[k] >= low_hz) lo = k;
        if (freq_axis_.bin_frequencies[k] <= high_hz) hi = k;
    }
    if (lo < 0 || hi < 0 || hi < lo) {
        out.frames_.clear();
        out.time_axis_.num_frames = 0;
        out.time_axis_.frame_times.clear();
        return out;
    }
    const int new_bins = hi - lo + 1;
    out.freq_axis_.num_bins = new_bins;
    out.freq_axis_.bin_frequencies.assign(
        freq_axis_.bin_frequencies.begin() + lo,
        freq_axis_.bin_frequencies.begin() + hi + 1);
    out.analysis_meta_.num_frequency_bins = new_bins;
    for (auto& f : out.frames_) {
        f.magnitudes.assign(
            f.magnitudes.begin() + lo,
            f.magnitudes.begin() + hi + 1);
        f.phases.assign(
            f.phases.begin() + lo,
            f.phases.begin() + hi + 1);
        f.power.assign(f.magnitudes.begin(), f.magnitudes.end());
        for (size_t k = 0; k < f.magnitudes.size(); ++k) {
            f.power[k] = f.magnitudes[k] * f.magnitudes[k];
        }
    }
    return out;
}

SpectralDataset SpectralDataset::filter_time(double start_sec, double end_sec) const {
    SpectralDataset out = *this;
    int s = 0, e = static_cast<int>(frames_.size());
    for (int i = 0; i < static_cast<int>(frames_.size()); ++i) {
        if (time_axis_.frame_times[i] < start_sec) s = i + 1;
        if (time_axis_.frame_times[i] > end_sec) { e = i; break; }
    }
    if (s >= e) {
        out.frames_.clear();
        out.time_axis_.num_frames = 0;
        out.time_axis_.frame_times.clear();
        return out;
    }
    out.frames_.assign(frames_.begin() + s, frames_.begin() + e);
    out.time_axis_.num_frames = static_cast<int>(out.frames_.size());
    out.time_axis_.total_duration = out.time_axis_.num_frames * out.time_axis_.frame_duration;
    out.time_axis_.frame_times.resize(out.frames_.size());
    for (int i = 0; i < out.time_axis_.num_frames; ++i) {
        out.frames_[i].frame_index = i;
        out.frames_[i].timestamp = i * out.time_axis_.frame_duration;
        out.time_axis_.frame_times[i] = out.frames_[i].timestamp;
    }
    out.analysis_meta_.total_frames = out.time_axis_.num_frames;
    out.analysis_meta_.total_duration_seconds = out.time_axis_.total_duration;
    return out;
}

SpectralDataset SpectralDataset::downsample_time(int factor) const {
    if (factor <= 1) return *this;
    SpectralDataset out = *this;
    out.frames_.clear();
    for (size_t i = 0; i < frames_.size(); i += factor) {
        out.frames_.push_back(frames_[i]);
    }
    out.time_axis_.num_frames = static_cast<int>(out.frames_.size());
    out.time_axis_.frame_duration = time_axis_.frame_duration * factor;
    out.time_axis_.total_duration = out.time_axis_.num_frames * out.time_axis_.frame_duration;
    out.time_axis_.frame_times.resize(out.frames_.size());
    for (int i = 0; i < out.time_axis_.num_frames; ++i) {
        out.frames_[i].frame_index = i;
        out.frames_[i].timestamp = i * out.time_axis_.frame_duration;
        out.time_axis_.frame_times[i] = out.frames_[i].timestamp;
    }
    out.analysis_meta_.total_frames = out.time_axis_.num_frames;
    out.analysis_meta_.total_duration_seconds = out.time_axis_.total_duration;
    return out;
}

SpectralDataset SpectralDataset::downsample_frequency(int factor) const {
    if (factor <= 1) return *this;
    SpectralDataset out = *this;
    int new_bins = 0;
    for (int k = 0; k < freq_axis_.num_bins; k += factor) ++new_bins;
    out.freq_axis_.num_bins = new_bins;
    out.freq_axis_.bin_frequencies.clear();
    for (int k = 0; k < freq_axis_.num_bins; k += factor) {
        out.freq_axis_.bin_frequencies.push_back(freq_axis_.bin_frequencies[k]);
    }
    out.freq_axis_.resolution = freq_axis_.resolution * factor;
    out.analysis_meta_.num_frequency_bins = new_bins;
    for (auto& f : out.frames_) {
        std::vector<float> nm, np;
        nm.reserve(new_bins);
        np.reserve(new_bins);
        for (int k = 0; k < freq_axis_.num_bins; k += factor) {
            nm.push_back(f.magnitudes[k]);
            np.push_back(f.phases[k]);
        }
        f.magnitudes = std::move(nm);
        f.phases = std::move(np);
        f.power.assign(f.magnitudes.begin(), f.magnitudes.end());
        for (size_t k = 0; k < f.magnitudes.size(); ++k) {
            f.power[k] = f.magnitudes[k] * f.magnitudes[k];
        }
    }
    return out;
}

// ============================================================================
// CSV export
// ============================================================================

bool SpectralDataset::export_csv(const std::string& path) const {
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << "frame_index,timestamp_s,freq_hz,magnitude,phase,power\n";
    ofs << std::fixed << std::setprecision(6);
    for (size_t i = 0; i < frames_.size(); ++i) {
        const auto& f = frames_[i];
        for (int k = 0; k < freq_axis_.num_bins; ++k) {
            const float mag = f.magnitudes[k];
            const float ph  = f.phases[k];
            ofs << f.frame_index << ',' << f.timestamp << ','
                << freq_axis_.bin_frequencies[k] << ','
                << mag << ',' << ph << ',' << (mag * mag) << '\n';
        }
    }
    return ofs.good();
}

// ============================================================================
// Binary serialization
// ============================================================================

void SpectralDataset::write_header(std::vector<uint8_t>& out) const {
    w_u32(out, BINARY_MAGIC);
    w_u32(out, version_);
    w_u32(out, BINARY_HEADER_SIZE);
    // Placeholders for sizes/checksum (filled after write)
    w_u64(out, 0); // payload size
    w_u64(out, 0); // checksum
    // Reserve rest of header
    out.resize(BINARY_HEADER_SIZE, 0);
}

bool SpectralDataset::read_header(const uint8_t* data, size_t size, size_t& off) {
    uint32_t magic = 0, version = 0, hdr_size = 0;
    if (!r_u32(data, size, off, magic)) return false;
    if (!r_u32(data, size, off, version)) return false;
    if (!r_u32(data, size, off, hdr_size)) return false;
    if (magic != BINARY_MAGIC) return false;
    if (hdr_size != BINARY_HEADER_SIZE) return false;
    // v3 gate: only the current explicit-LE layout loads. Older blobs
    // (native-POD v1/v2) are rejected rather than misinterpreted.
    if (version < SPECTRAL_DATASET_MIN_COMPATIBLE_VERSION) return false;
    if (version > SPECTRAL_DATASET_VERSION) return false;
    version_ = version;
    off = BINARY_HEADER_SIZE;
    return true;
}

void SpectralDataset::write_metadata(std::vector<uint8_t>& out) const {
    // Source metadata
    w_str(out, source_meta_.file_path);
    w_str(out, source_meta_.file_hash);
    w_u64(out, source_meta_.file_size_bytes);
    w_i32(out, source_meta_.sample_rate);
    w_i32(out, source_meta_.num_channels);
    w_f64(out, source_meta_.duration_seconds);
    w_str(out, source_meta_.codec_name);
    w_str(out, source_meta_.codec_long_name);
    // Analysis metadata
    w_str(out, analysis_meta_.window_type);
    w_f32(out, analysis_meta_.window_coherent_gain);
    w_i32(out, analysis_meta_.fft_size);
    w_i32(out, analysis_meta_.hop_size);
    w_f32(out, analysis_meta_.overlap_ratio);
    w_i32(out, analysis_meta_.sample_rate);
    w_i32(out, analysis_meta_.analyzed_channels);
    w_i32(out, analysis_meta_.channel_mapping);
    w_f32(out, analysis_meta_.magnitude_scale);
    w_f32(out, analysis_meta_.phase_unwrap);
    w_f32(out, analysis_meta_.nyquist_frequency);
    w_i32(out, analysis_meta_.num_frequency_bins);
    w_f64(out, analysis_meta_.frame_duration_seconds);
    w_f64(out, analysis_meta_.total_duration_seconds);
    w_i32(out, analysis_meta_.total_frames);
    w_str(out, analysis_meta_.analyzer_version);
    w_str(out, analysis_meta_.analysis_method);
    w_i32(out, analysis_meta_.band_count);
    // Normalization
    w_f32(out, normalization_.window_coherent_gain);
    w_f32(out, normalization_.window_energy_gain);
    w_f32(out, normalization_.magnitude_scale);
    w_f32(out, normalization_.reference_amplitude);
    w_f32(out, normalization_.db_floor);
    w_f32(out, normalization_.db_reference);
    w_bool(out, normalization_.phase_unwrapped);
    w_f32(out, normalization_.phase_reference);
    w_bool(out, normalization_.channel_normalized);
    // Channel info
    w_i32(out, channel_info_.total_channels);
    w_i32(out, channel_info_.analyzed_channels);
    w_i32(out, channel_info_.analyzed_channel_index);
    w_bool(out, channel_info_.channels_mixed);
    w_u32(out, static_cast<uint32_t>(channel_info_.channel_names.size()));
    for (const auto& n : channel_info_.channel_names) w_str(out, n);
}

bool SpectralDataset::read_metadata(const uint8_t* data, size_t size, size_t& off) {
    if (!r_str(data, size, off, source_meta_.file_path)) return false;
    if (!r_str(data, size, off, source_meta_.file_hash)) return false;
    uint64_t fsize = 0;
    if (!r_u64(data, size, off, fsize)) return false;
    source_meta_.file_size_bytes = fsize;
    if (!r_i32(data, size, off, source_meta_.sample_rate)) return false;
    if (!r_i32(data, size, off, source_meta_.num_channels)) return false;
    if (!r_f64(data, size, off, source_meta_.duration_seconds)) return false;
    if (!r_str(data, size, off, source_meta_.codec_name)) return false;
    if (!r_str(data, size, off, source_meta_.codec_long_name)) return false;

    if (!r_str(data, size, off, analysis_meta_.window_type)) return false;
    if (!r_f32(data, size, off, analysis_meta_.window_coherent_gain)) return false;
    if (!r_i32(data, size, off, analysis_meta_.fft_size)) return false;
    if (!r_i32(data, size, off, analysis_meta_.hop_size)) return false;
    if (!r_f32(data, size, off, analysis_meta_.overlap_ratio)) return false;
    if (!r_i32(data, size, off, analysis_meta_.sample_rate)) return false;
    if (!r_i32(data, size, off, analysis_meta_.analyzed_channels)) return false;
    if (!r_i32(data, size, off, analysis_meta_.channel_mapping)) return false;
    if (!r_f32(data, size, off, analysis_meta_.magnitude_scale)) return false;
    if (!r_f32(data, size, off, analysis_meta_.phase_unwrap)) return false;
    if (!r_f32(data, size, off, analysis_meta_.nyquist_frequency)) return false;
    if (!r_i32(data, size, off, analysis_meta_.num_frequency_bins)) return false;
    if (!r_f64(data, size, off, analysis_meta_.frame_duration_seconds)) return false;
    if (!r_f64(data, size, off, analysis_meta_.total_duration_seconds)) return false;
    if (!r_i32(data, size, off, analysis_meta_.total_frames)) return false;
    if (!r_str(data, size, off, analysis_meta_.analyzer_version)) return false;
    if (!r_str(data, size, off, analysis_meta_.analysis_method)) return false;
    if (!r_i32(data, size, off, analysis_meta_.band_count)) return false;

    if (!r_f32(data, size, off, normalization_.window_coherent_gain)) return false;
    if (!r_f32(data, size, off, normalization_.window_energy_gain)) return false;
    if (!r_f32(data, size, off, normalization_.magnitude_scale)) return false;
    if (!r_f32(data, size, off, normalization_.reference_amplitude)) return false;
    if (!r_f32(data, size, off, normalization_.db_floor)) return false;
    if (!r_f32(data, size, off, normalization_.db_reference)) return false;
    if (!r_bool(data, size, off, normalization_.phase_unwrapped)) return false;
    if (!r_f32(data, size, off, normalization_.phase_reference)) return false;
    if (!r_bool(data, size, off, normalization_.channel_normalized)) return false;

    if (!r_i32(data, size, off, channel_info_.total_channels)) return false;
    if (!r_i32(data, size, off, channel_info_.analyzed_channels)) return false;
    if (!r_i32(data, size, off, channel_info_.analyzed_channel_index)) return false;
    if (!r_bool(data, size, off, channel_info_.channels_mixed)) return false;
    uint32_t n_names = 0;
    if (!r_u32(data, size, off, n_names)) return false;
    if (n_names > MAX_NAMES) return false;
    channel_info_.channel_names.clear();
    for (uint32_t i = 0; i < n_names; ++i) {
        std::string n;
        if (!r_str(data, size, off, n)) return false;
        channel_info_.channel_names.push_back(n);
    }
    return true;
}

void SpectralDataset::write_axes(std::vector<uint8_t>& out) const {
    // Frequency axis
    w_i32(out, freq_axis_.num_bins);
    w_i32(out, freq_axis_.fft_size);
    w_i32(out, freq_axis_.sample_rate);
    w_f32(out, freq_axis_.nyquist);
    w_f32(out, freq_axis_.resolution);
    w_u64(out, static_cast<uint64_t>(freq_axis_.bin_frequencies.size()));
    for (float f : freq_axis_.bin_frequencies) w_f32(out, f);
    // Time axis
    w_i32(out, time_axis_.num_frames);
    w_i32(out, time_axis_.hop_size);
    w_i32(out, time_axis_.sample_rate);
    w_f64(out, time_axis_.frame_duration);
    w_f64(out, time_axis_.total_duration);
    w_u64(out, static_cast<uint64_t>(time_axis_.frame_times.size()));
    for (double t : time_axis_.frame_times) w_f64(out, t);
}

bool SpectralDataset::read_axes(const uint8_t* data, size_t size, size_t& off) {
    if (!r_i32(data, size, off, freq_axis_.num_bins)) return false;
    if (!r_i32(data, size, off, freq_axis_.fft_size)) return false;
    if (!r_i32(data, size, off, freq_axis_.sample_rate)) return false;
    if (!r_f32(data, size, off, freq_axis_.nyquist)) return false;
    if (!r_f32(data, size, off, freq_axis_.resolution)) return false;
    uint64_t nfreq = 0;
    if (!r_u64(data, size, off, nfreq)) return false;
    if (nfreq > MAX_BINS + 1) return false;
    freq_axis_.bin_frequencies.resize(static_cast<size_t>(nfreq));
    for (uint64_t i = 0; i < nfreq; ++i) {
        if (!r_f32(data, size, off, freq_axis_.bin_frequencies[static_cast<size_t>(i)])) return false;
    }

    if (!r_i32(data, size, off, time_axis_.num_frames)) return false;
    if (!r_i32(data, size, off, time_axis_.hop_size)) return false;
    if (!r_i32(data, size, off, time_axis_.sample_rate)) return false;
    if (!r_f64(data, size, off, time_axis_.frame_duration)) return false;
    if (!r_f64(data, size, off, time_axis_.total_duration)) return false;
    uint64_t ntimes = 0;
    if (!r_u64(data, size, off, ntimes)) return false;
    if (ntimes > MAX_FRAMES) return false;
    time_axis_.frame_times.resize(static_cast<size_t>(ntimes));
    for (uint64_t i = 0; i < ntimes; ++i) {
        if (!r_f64(data, size, off, time_axis_.frame_times[static_cast<size_t>(i)])) return false;
    }
    return true;
}

void SpectralDataset::write_frames(std::vector<uint8_t>& out) const {
    w_u64(out, static_cast<uint64_t>(frames_.size()));
    for (const auto& f : frames_) {
        w_i32(out, f.frame_index);
        w_i32(out, f.n_fft);
        w_f32(out, f.window_factor);
        w_f64(out, f.timestamp);
        w_f32(out, f.rms);
        w_f32(out, f.peak_magnitude);
        w_f32(out, f.spectral_centroid);
        w_f32(out, f.spectral_bandwidth);
        w_i32(out, f.band_count);
        w_u64(out, static_cast<uint64_t>(f.magnitudes.size()));
        for (float m : f.magnitudes) w_f32(out, m);
        w_u64(out, static_cast<uint64_t>(f.phases.size()));
        for (float p : f.phases) w_f32(out, p);
        w_u64(out, static_cast<uint64_t>(f.power.size()));
        for (float p : f.power) w_f32(out, p);
        // Reassigned coordinates (v3; empty for conventional STFT).
        w_u64(out, static_cast<uint64_t>(f.reassigned_times.size()));
        for (float t : f.reassigned_times) w_f32(out, t);
        w_u64(out, static_cast<uint64_t>(f.reassigned_freqs.size()));
        for (float fr : f.reassigned_freqs) w_f32(out, fr);
    }
}

bool SpectralDataset::read_frames(const uint8_t* data, size_t size, size_t& off) {
    uint64_t n = 0;
    if (!r_u64(data, size, off, n)) return false;
    if (n > MAX_FRAMES) return false;
    const uint64_t nb = freq_axis_.num_bins > 0
                            ? static_cast<uint64_t>(freq_axis_.num_bins)
                            : MAX_BINS;
    frames_.clear();
    frames_.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        SpectralFrame f;
        if (!r_i32(data, size, off, f.frame_index)) return false;
        if (!r_i32(data, size, off, f.n_fft)) return false;
        if (!r_f32(data, size, off, f.window_factor)) return false;
        if (!r_f64(data, size, off, f.timestamp)) return false;
        if (!r_f32(data, size, off, f.rms)) return false;
        if (!r_f32(data, size, off, f.peak_magnitude)) return false;
        if (!r_f32(data, size, off, f.spectral_centroid)) return false;
        if (!r_f32(data, size, off, f.spectral_bandwidth)) return false;
        if (!r_i32(data, size, off, f.band_count)) return false;
        uint64_t nm = 0, np = 0, npo = 0, nrt = 0, nrf = 0;
        if (!r_u64(data, size, off, nm)) return false;
        // v3 strict: spectral arrays match the declared bin count.
        if (nm != nb) return false;
        if (!read_f32_array(data, size, off, nm, MAX_BINS, f.magnitudes)) return false;
        if (!r_u64(data, size, off, np)) return false;
        if (np != nb) return false;
        if (!read_f32_array(data, size, off, np, MAX_BINS, f.phases)) return false;
        if (!r_u64(data, size, off, npo)) return false;
        if (npo != nb) return false;
        if (!read_f32_array(data, size, off, npo, MAX_BINS, f.power)) return false;
        if (!r_u64(data, size, off, nrt)) return false;
        if (nrt != 0 && nrt != nb) return false;
        if (!read_f32_array(data, size, off, nrt, MAX_BINS, f.reassigned_times))
            return false;
        if (!r_u64(data, size, off, nrf)) return false;
        if (nrf != 0 && nrf != nb) return false;
        if (!read_f32_array(data, size, off, nrf, MAX_BINS, f.reassigned_freqs))
            return false;
        frames_.push_back(std::move(f));
    }
    return true;
}

bool SpectralDataset::serialize_binary(std::vector<uint8_t>& out) const {
    out.clear();
    // Rough reserve — header + metadata + axes + frames (mag/phs/pwr +
    // reassigned pair). Avoids repeated realloc during append.
    const size_t est = BINARY_HEADER_SIZE + 1024 +
                        frames_.size() * (80 + num_frequency_bins() * 20);
    out.reserve(est);

    // Header
    write_header(out);

    // Payload
    const size_t payload_start = out.size();
    write_metadata(out);
    write_axes(out);
    write_frames(out);
    const size_t payload_end = out.size();
    const uint64_t payload_size = payload_end - payload_start;

    // Compute checksum over payload
    const uint64_t checksum = fnv1a_64(out.data() + payload_start, payload_size);

    // Patch header fields: payload size and checksum (explicit LE).
    for (int i = 0; i < 8; ++i) {
        out[12 + i] = static_cast<uint8_t>(payload_size >> (8 * i));
        out[20 + i] = static_cast<uint8_t>(checksum >> (8 * i));
    }

    if (out.size() > MAX_DATASET_SIZE) return false;
    return true;
}

bool SpectralDataset::deserialize_binary(const uint8_t* data, size_t size) {
    if (size < BINARY_HEADER_SIZE) return false;
    if (size > MAX_DATASET_SIZE) return false;

    // Verify header
    size_t off = 0;
    if (!read_header(data, size, off)) return false;

    // Read claimed payload size + checksum (explicit LE).
    uint64_t payload_size = 0, claimed_checksum = 0;
    for (int i = 0; i < 8; ++i) {
        payload_size |= static_cast<uint64_t>(data[12 + i]) << (8 * i);
        claimed_checksum |= static_cast<uint64_t>(data[20 + i]) << (8 * i);
    }
    if (payload_size > size - BINARY_HEADER_SIZE) return false;
    if (BINARY_HEADER_SIZE + payload_size > size) return false;

    // Verify checksum BEFORE replacing state
    const uint64_t actual = fnv1a_64(
        data + BINARY_HEADER_SIZE, static_cast<size_t>(payload_size));
    if (actual != claimed_checksum) return false;

    off = BINARY_HEADER_SIZE;
    if (!read_metadata(data, BINARY_HEADER_SIZE + payload_size, off)) return false;
    if (!read_axes(data, BINARY_HEADER_SIZE + payload_size, off)) return false;
    if (!read_frames(data, BINARY_HEADER_SIZE + payload_size, off)) return false;

    return true;
}

std::string SpectralDataset::dataset_identity() const {
    std::vector<uint8_t> buf;
    if (!serialize_binary(buf)) return {};
    return sha256_hex(buf.data(), buf.size());
}

// ============================================================================
// JSON serialization
// ============================================================================

namespace {

void write_json_metadata(std::ostringstream& os, const SpectralDataset& d) {
    os << "  \"source\": {\n";
    os << "    \"file_path\": \"" << json_escape(d.source_metadata().file_path) << "\",\n";
    os << "    \"file_hash\": \"" << json_escape(d.source_metadata().file_hash) << "\",\n";
    os << "    \"file_size_bytes\": " << d.source_metadata().file_size_bytes << ",\n";
    os << "    \"sample_rate\": " << d.source_metadata().sample_rate << ",\n";
    os << "    \"num_channels\": " << d.source_metadata().num_channels << ",\n";
    os << "    \"duration_seconds\": " << fp_to_string(d.source_metadata().duration_seconds) << ",\n";
    os << "    \"codec_name\": \"" << json_escape(d.source_metadata().codec_name) << "\",\n";
    os << "    \"codec_long_name\": \"" << json_escape(d.source_metadata().codec_long_name) << "\"\n";
    os << "  },\n";

    os << "  \"analysis\": {\n";
    os << "    \"window_type\": \"" << json_escape(d.analysis_metadata().window_type) << "\",\n";
    os << "    \"window_coherent_gain\": " << fp_to_string(d.analysis_metadata().window_coherent_gain) << ",\n";
    os << "    \"fft_size\": " << d.analysis_metadata().fft_size << ",\n";
    os << "    \"hop_size\": " << d.analysis_metadata().hop_size << ",\n";
    os << "    \"overlap_ratio\": " << fp_to_string(d.analysis_metadata().overlap_ratio) << ",\n";
    os << "    \"sample_rate\": " << d.analysis_metadata().sample_rate << ",\n";
    os << "    \"analyzed_channels\": " << d.analysis_metadata().analyzed_channels << ",\n";
    os << "    \"channel_mapping\": " << d.analysis_metadata().channel_mapping << ",\n";
    os << "    \"magnitude_scale\": " << fp_to_string(d.analysis_metadata().magnitude_scale) << ",\n";
    os << "    \"phase_unwrap\": " << fp_to_string(d.analysis_metadata().phase_unwrap) << ",\n";
    os << "    \"nyquist_frequency\": " << fp_to_string(d.analysis_metadata().nyquist_frequency) << ",\n";
    os << "    \"num_frequency_bins\": " << d.analysis_metadata().num_frequency_bins << ",\n";
    os << "    \"frame_duration_seconds\": " << fp_to_string(d.analysis_metadata().frame_duration_seconds) << ",\n";
    os << "    \"total_duration_seconds\": " << fp_to_string(d.analysis_metadata().total_duration_seconds) << ",\n";
    os << "    \"total_frames\": " << d.analysis_metadata().total_frames << ",\n";
    os << "    \"analyzer_version\": \"" << json_escape(d.analysis_metadata().analyzer_version) << "\"\n";
    os << "  },\n";

    os << "  \"normalization\": {\n";
    os << "    \"window_coherent_gain\": " << fp_to_string(d.normalization_info().window_coherent_gain) << ",\n";
    os << "    \"window_energy_gain\": " << fp_to_string(d.normalization_info().window_energy_gain) << ",\n";
    os << "    \"magnitude_scale\": " << fp_to_string(d.normalization_info().magnitude_scale) << ",\n";
    os << "    \"reference_amplitude\": " << fp_to_string(d.normalization_info().reference_amplitude) << ",\n";
    os << "    \"db_floor\": " << fp_to_string(d.normalization_info().db_floor) << ",\n";
    os << "    \"db_reference\": " << fp_to_string(d.normalization_info().db_reference) << ",\n";
    os << "    \"phase_unwrapped\": " << (d.normalization_info().phase_unwrapped ? "true" : "false") << ",\n";
    os << "    \"phase_reference\": " << fp_to_string(d.normalization_info().phase_reference) << ",\n";
    os << "    \"channel_normalized\": " << (d.normalization_info().channel_normalized ? "true" : "false") << "\n";
    os << "  },\n";

    os << "  \"channel_info\": {\n";
    os << "    \"total_channels\": " << d.channel_info().total_channels << ",\n";
    os << "    \"analyzed_channels\": " << d.channel_info().analyzed_channels << ",\n";
    os << "    \"analyzed_channel_index\": " << d.channel_info().analyzed_channel_index << ",\n";
    os << "    \"channels_mixed\": " << (d.channel_info().channels_mixed ? "true" : "false") << ",\n";
    os << "    \"channel_names\": [";
    for (size_t i = 0; i < d.channel_info().channel_names.size(); ++i) {
        if (i) os << ",";
        os << "\"" << json_escape(d.channel_info().channel_names[i]) << "\"";
    }
    os << "]\n";
    os << "  },\n";
    // Representation contract (S6.0; JSON-persisted, binary waits for v4).
    const auto& rep = d.representation();
    os << "  \"representation\": {\n";
    os << "    \"kind\": \"" << representation_kind_name(rep.kind) << "\",\n";
    os << "    \"bins\": " << rep.bins << ",\n";
    os << "    \"fmin_hz\": " << fp_to_string(rep.fmin_hz) << ",\n";
    os << "    \"fmax_hz\": " << fp_to_string(rep.fmax_hz) << ",\n";
    os << "    \"bands\": " << rep.bands << ",\n";
    os << "    \"q\": " << fp_to_string(rep.q) << ",\n";
    os << "    \"norm\": \"" << representation_norm_name(rep.norm) << "\",\n";
    os << "    \"phase\": \"" << (rep.phase == RepresentationPhase::Available ? "available" : "n/a") << "\",\n";
    os << "    \"reassignment\": " << (rep.reassignment_supported ? "true" : "false") << ",\n";
    os << "    \"version\": " << rep.version << "\n";
    os << "  }\n";
}

} // namespace

std::string SpectralDataset::serialize_json(bool pretty) const {
    std::ostringstream os;
    os << "{\n";
    os << "  \"version\": " << version_ << ",\n";
    os << "  \"min_compatible_version\": " << SPECTRAL_DATASET_MIN_COMPATIBLE_VERSION << ",\n";
    write_json_metadata(os, *this);
    os << ",\n  \"frequency_axis\": {\n";
    os << "    \"num_bins\": " << freq_axis_.num_bins << ",\n";
    os << "    \"fft_size\": " << freq_axis_.fft_size << ",\n";
    os << "    \"sample_rate\": " << freq_axis_.sample_rate << ",\n";
    os << "    \"nyquist\": " << fp_to_string(freq_axis_.nyquist) << ",\n";
    os << "    \"resolution\": " << fp_to_string(freq_axis_.resolution) << ",\n";
    os << "    \"bin_frequencies\": [";
    for (size_t i = 0; i < freq_axis_.bin_frequencies.size(); ++i) {
        if (i) os << ",";
        os << fp_to_string(freq_axis_.bin_frequencies[i]);
    }
    os << "]\n";
    os << "  },\n";
    os << "  \"time_axis\": {\n";
    os << "    \"num_frames\": " << time_axis_.num_frames << ",\n";
    os << "    \"hop_size\": " << time_axis_.hop_size << ",\n";
    os << "    \"sample_rate\": " << time_axis_.sample_rate << ",\n";
    os << "    \"frame_duration\": " << fp_to_string(time_axis_.frame_duration) << ",\n";
    os << "    \"total_duration\": " << fp_to_string(time_axis_.total_duration) << ",\n";
    os << "    \"frame_times\": [";
    for (size_t i = 0; i < time_axis_.frame_times.size(); ++i) {
        if (i) os << ",";
        os << fp_to_string(time_axis_.frame_times[i]);
    }
    os << "]\n";
    os << "  },\n";
    os << "  \"frames\": [\n";
    for (size_t i = 0; i < frames_.size(); ++i) {
        const auto& f = frames_[i];
        os << "    {\n";
        os << "      \"frame_index\": " << f.frame_index << ",\n";
        os << "      \"n_fft\": " << f.n_fft << ",\n";
        os << "      \"window_factor\": " << fp_to_string(f.window_factor) << ",\n";
        os << "      \"timestamp\": " << fp_to_string(f.timestamp) << ",\n";
        os << "      \"rms\": " << fp_to_string(f.rms) << ",\n";
        os << "      \"peak_magnitude\": " << fp_to_string(f.peak_magnitude) << ",\n";
        os << "      \"spectral_centroid\": " << fp_to_string(f.spectral_centroid) << ",\n";
        os << "      \"spectral_bandwidth\": " << fp_to_string(f.spectral_bandwidth) << ",\n";
        os << "      \"magnitudes\": [";
        for (size_t k = 0; k < f.magnitudes.size(); ++k) {
            if (k) os << ",";
            os << fp_to_string(f.magnitudes[k]);
        }
        os << "],\n";
        os << "      \"phases\": [";
        for (size_t k = 0; k < f.phases.size(); ++k) {
            if (k) os << ",";
            os << fp_to_string(f.phases[k]);
        }
        os << "],\n";
        os << "      \"power\": [";
        for (size_t k = 0; k < f.power.size(); ++k) {
            if (k) os << ",";
            os << fp_to_string(f.power[k]);
        }
        os << "],\n";
        os << "      \"reassigned_times\": [";
        for (size_t k = 0; k < f.reassigned_times.size(); ++k) {
            if (k) os << ",";
            os << fp_to_string(f.reassigned_times[k]);
        }
        os << "],\n";
        os << "      \"reassigned_freqs\": [";
        for (size_t k = 0; k < f.reassigned_freqs.size(); ++k) {
            if (k) os << ",";
            os << fp_to_string(f.reassigned_freqs[k]);
        }
        os << "]\n";
        os << "    }" << (i + 1 < frames_.size() ? "," : "") << "\n";
    }
    os << "  ]\n";
    os << "}\n";
    (void)pretty;
    return os.str();
}

bool SpectralDataset::deserialize_json(const std::string& json) {
    // Section-aware JSON parser. We walk the top-level object and locate
    // each known section ("source", "analysis", ...). Within each section
    // we extract only the keys that section owns, so duplicate keys across
    // sections (e.g. "fft_size" in both analysis and frequency_axis) are
    // routed correctly.

    auto find_in_range = [&](const std::string& key,
                              const std::string& s,
                              size_t start) -> std::string {
        const std::string k = "\"" + key + "\"";
        size_t p = s.find(k, start);
        if (p == std::string::npos) return {};
        p = s.find(':', p);
        if (p == std::string::npos) return {};
        ++p;
        while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
        return s.substr(p);
    };

    auto section_range = [&](const std::string& key) -> std::pair<size_t,size_t> {
        // Return [begin, end) of the OBJECT body for top-level key
        const std::string k = "\"" + key + "\"";
        size_t p = json.find(k);
        if (p == std::string::npos) return {std::string::npos, std::string::npos};
        p = json.find('{', p);
        if (p == std::string::npos) return {std::string::npos, std::string::npos};
        int depth = 0;
        size_t q = p;
        for (; q < json.size(); ++q) {
            if (json[q] == '{') ++depth;
            else if (json[q] == '}') { --depth; if (depth == 0) { ++q; break; } }
        }
        if (depth != 0) return {std::string::npos, std::string::npos};
        return {p, q};
    };

    auto extract_string = [](const std::string& s) -> std::string {
        size_t a = s.find('"');
        if (a == std::string::npos) return {};
        size_t b = s.find('"', a + 1);
        if (b == std::string::npos) return {};
        return json_unescape(s.substr(a + 1, b - a - 1));
    };

    auto extract_number = [](const std::string& s) -> double {
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) ||
               s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '-' || s[i] == '+')) {
            ++i;
        }
        return parse_json_number(s.substr(start, i - start));
    };

    auto extract_bool = [](const std::string& s) -> bool {
        return s.find("true") != std::string::npos;
    };

    auto extract_array_floats = [&](const std::string& s) -> std::vector<float> {
        std::vector<float> out;
        size_t a = s.find('[');
        size_t b = s.find(']', a);
        if (a == std::string::npos || b == std::string::npos) return out;
        std::string body = s.substr(a + 1, b - a - 1);
        std::string cur;
        for (char c : body) {
            if (c == ',') {
                if (!cur.empty()) {
                    double v = parse_json_number(cur);
                    if (std::isnan(v)) return {};
                    out.push_back(static_cast<float>(v));
                    cur.clear();
                }
            } else if (!std::isspace(static_cast<unsigned char>(c))) {
                cur += c;
            }
        }
        if (!cur.empty()) {
            double v = parse_json_number(cur);
            if (std::isnan(v)) return {};
            out.push_back(static_cast<float>(v));
        }
        return out;
    };

    auto extract_array_doubles = [&](const std::string& s) -> std::vector<double> {
        std::vector<double> out;
        size_t a = s.find('[');
        size_t b = s.find(']', a);
        if (a == std::string::npos || b == std::string::npos) return out;
        std::string body = s.substr(a + 1, b - a - 1);
        std::string cur;
        for (char c : body) {
            if (c == ',') {
                if (!cur.empty()) {
                    double v = parse_json_number(cur);
                    if (std::isnan(v)) return {};
                    out.push_back(v);
                    cur.clear();
                }
            } else if (!std::isspace(static_cast<unsigned char>(c))) {
                cur += c;
            }
        }
        if (!cur.empty()) {
            double v = parse_json_number(cur);
            if (std::isnan(v)) return {};
            out.push_back(v);
        }
        return out;
    };

    auto extract_array_strings = [&](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> out;
        size_t a = s.find('[');
        size_t b = s.find(']', a);
        if (a == std::string::npos || b == std::string::npos) return out;
        std::string body = s.substr(a + 1, b - a - 1);
        size_t i = 0;
        while (i < body.size()) {
            if (body[i] == '"') {
                size_t j = i + 1;
                std::string cur;
                while (j < body.size() && body[j] != '"') {
                    if (body[j] == '\\' && j + 1 < body.size()) {
                        cur += body[j];
                        cur += body[j + 1];
                        j += 2;
                    } else {
                        cur += body[j++];
                    }
                }
                out.push_back(json_unescape(cur));
                i = j + 1;
            } else {
                ++i;
            }
        }
        return out;
    };

    // ---- Version (top-level scalar) ----
    {
        const std::string k = "\"version\"";
        size_t p = json.find(k);
        if (p == std::string::npos) return false;
        p = json.find(':', p);
        if (p == std::string::npos) return false;
        ++p;
        while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
        uint32_t ver = static_cast<uint32_t>(extract_number(json.substr(p)));
        if (ver < SPECTRAL_DATASET_MIN_COMPATIBLE_VERSION) return false;
        if (ver > SPECTRAL_DATASET_VERSION) return false;
        version_ = ver;
    }

    // ---- source section ----
    {
        auto [a, b] = section_range("source");
        if (a == std::string::npos) return false;
        const std::string sec = json.substr(a, b - a);
        source_meta_.file_path =
            extract_string(find_in_range("file_path", sec, 0));
        source_meta_.file_hash =
            extract_string(find_in_range("file_hash", sec, 0));
        source_meta_.file_size_bytes = static_cast<uint64_t>(
            extract_number(find_in_range("file_size_bytes", sec, 0)));
        source_meta_.sample_rate = static_cast<int>(
            extract_number(find_in_range("sample_rate", sec, 0)));
        source_meta_.num_channels = static_cast<int>(
            extract_number(find_in_range("num_channels", sec, 0)));
        source_meta_.duration_seconds =
            extract_number(find_in_range("duration_seconds", sec, 0));
        source_meta_.codec_name =
            extract_string(find_in_range("codec_name", sec, 0));
        source_meta_.codec_long_name =
            extract_string(find_in_range("codec_long_name", sec, 0));
    }

    // ---- analysis section ----
    {
        auto [a, b] = section_range("analysis");
        if (a == std::string::npos) return false;
        const std::string sec = json.substr(a, b - a);
        analysis_meta_.window_type =
            extract_string(find_in_range("window_type", sec, 0));
        analysis_meta_.window_coherent_gain = static_cast<float>(
            extract_number(find_in_range("window_coherent_gain", sec, 0)));
        analysis_meta_.fft_size = static_cast<int>(
            extract_number(find_in_range("fft_size", sec, 0)));
        analysis_meta_.hop_size = static_cast<int>(
            extract_number(find_in_range("hop_size", sec, 0)));
        analysis_meta_.overlap_ratio = static_cast<float>(
            extract_number(find_in_range("overlap_ratio", sec, 0)));
        analysis_meta_.sample_rate = static_cast<int>(
            extract_number(find_in_range("sample_rate", sec, 0)));
        analysis_meta_.analyzed_channels = static_cast<int>(
            extract_number(find_in_range("analyzed_channels", sec, 0)));
        analysis_meta_.channel_mapping = static_cast<int>(
            extract_number(find_in_range("channel_mapping", sec, 0)));
        analysis_meta_.magnitude_scale = static_cast<float>(
            extract_number(find_in_range("magnitude_scale", sec, 0)));
        analysis_meta_.phase_unwrap = static_cast<float>(
            extract_number(find_in_range("phase_unwrap", sec, 0)));
        analysis_meta_.nyquist_frequency = static_cast<float>(
            extract_number(find_in_range("nyquist_frequency", sec, 0)));
        analysis_meta_.num_frequency_bins = static_cast<int>(
            extract_number(find_in_range("num_frequency_bins", sec, 0)));
        analysis_meta_.frame_duration_seconds =
            extract_number(find_in_range("frame_duration_seconds", sec, 0));
        analysis_meta_.total_duration_seconds =
            extract_number(find_in_range("total_duration_seconds", sec, 0));
        analysis_meta_.total_frames = static_cast<int>(
            extract_number(find_in_range("total_frames", sec, 0)));
        analysis_meta_.analyzer_version =
            extract_string(find_in_range("analyzer_version", sec, 0));
    }

    // ---- normalization section ----
    {
        auto [a, b] = section_range("normalization");
        if (a == std::string::npos) return false;
        const std::string sec = json.substr(a, b - a);
        normalization_.window_coherent_gain = static_cast<float>(
            extract_number(find_in_range("window_coherent_gain", sec, 0)));
        normalization_.window_energy_gain = static_cast<float>(
            extract_number(find_in_range("window_energy_gain", sec, 0)));
        normalization_.magnitude_scale = static_cast<float>(
            extract_number(find_in_range("magnitude_scale", sec, 0)));
        normalization_.reference_amplitude = static_cast<float>(
            extract_number(find_in_range("reference_amplitude", sec, 0)));
        normalization_.db_floor = static_cast<float>(
            extract_number(find_in_range("db_floor", sec, 0)));
        normalization_.db_reference = static_cast<float>(
            extract_number(find_in_range("db_reference", sec, 0)));
        normalization_.phase_unwrapped =
            extract_bool(find_in_range("phase_unwrapped", sec, 0));
        normalization_.phase_reference = static_cast<float>(
            extract_number(find_in_range("phase_reference", sec, 0)));
        normalization_.channel_normalized =
            extract_bool(find_in_range("channel_normalized", sec, 0));
    }

    // ---- channel_info section ----
    {
        auto [a, b] = section_range("channel_info");
        if (a == std::string::npos) return false;
        const std::string sec = json.substr(a, b - a);
        channel_info_.total_channels = static_cast<int>(
            extract_number(find_in_range("total_channels", sec, 0)));
        channel_info_.analyzed_channels = static_cast<int>(
            extract_number(find_in_range("analyzed_channels", sec, 0)));
        channel_info_.analyzed_channel_index = static_cast<int>(
            extract_number(find_in_range("analyzed_channel_index", sec, 0)));
        channel_info_.channels_mixed =
            extract_bool(find_in_range("channels_mixed", sec, 0));
        channel_info_.channel_names =
            extract_array_strings(find_in_range("channel_names", sec, 0));
    }

    // ---- representation section (absent in pre-S6.0 JSON: STFT default) ----
    {
        auto [a, b] = section_range("representation");
        if (a != std::string::npos) {
            const std::string sec = json.substr(a, b - a);
            auto grab = [&](const std::string& k) { return find_in_range(k, sec, 0); };
            RepresentationKind k = RepresentationKind::STFT;
            if (!try_parse_representation_kind(extract_string(grab("kind")), k)) return false;
            representation_.kind = k;
            representation_.bins = static_cast<int>(extract_number(grab("bins")));
            representation_.fmin_hz = static_cast<float>(extract_number(grab("fmin_hz")));
            representation_.fmax_hz = static_cast<float>(extract_number(grab("fmax_hz")));
            representation_.bands = static_cast<int>(extract_number(grab("bands")));
            representation_.q = static_cast<float>(extract_number(grab("q")));
            RepresentationNorm n = RepresentationNorm::None;
            if (!try_parse_representation_norm(extract_string(grab("norm")), n)) return false;
            representation_.norm = n;
            const std::string ph = extract_string(grab("phase"));
            if (ph != "available" && ph != "n/a") return false;
            representation_.phase = (ph == "available") ? RepresentationPhase::Available
                                                          : RepresentationPhase::NotApplicable;
            representation_.reassignment_supported = extract_bool(grab("reassignment"));
            representation_.version = static_cast<uint32_t>(extract_number(grab("version")));
        }
    }

    // ---- frequency_axis section ----
    {
        auto [a, b] = section_range("frequency_axis");
        if (a == std::string::npos) return false;
        const std::string sec = json.substr(a, b - a);
        freq_axis_.num_bins = static_cast<int>(
            extract_number(find_in_range("num_bins", sec, 0)));
        freq_axis_.fft_size = static_cast<int>(
            extract_number(find_in_range("fft_size", sec, 0)));
        freq_axis_.sample_rate = static_cast<int>(
            extract_number(find_in_range("sample_rate", sec, 0)));
        freq_axis_.nyquist = static_cast<float>(
            extract_number(find_in_range("nyquist", sec, 0)));
        freq_axis_.resolution = static_cast<float>(
            extract_number(find_in_range("resolution", sec, 0)));
        freq_axis_.bin_frequencies =
            extract_array_floats(find_in_range("bin_frequencies", sec, 0));
    }

    // ---- time_axis section ----
    {
        auto [a, b] = section_range("time_axis");
        if (a == std::string::npos) return false;
        const std::string sec = json.substr(a, b - a);
        time_axis_.num_frames = static_cast<int>(
            extract_number(find_in_range("num_frames", sec, 0)));
        time_axis_.hop_size = static_cast<int>(
            extract_number(find_in_range("hop_size", sec, 0)));
        time_axis_.sample_rate = static_cast<int>(
            extract_number(find_in_range("sample_rate", sec, 0)));
        time_axis_.frame_duration =
            extract_number(find_in_range("frame_duration", sec, 0));
        time_axis_.total_duration =
            extract_number(find_in_range("total_duration", sec, 0));
        time_axis_.frame_times =
            extract_array_doubles(find_in_range("frame_times", sec, 0));
    }

    // ---- frames section ----
    frames_.clear();
    {
        size_t frames_pos = json.find("\"frames\"");
        if (frames_pos == std::string::npos) return false;
        size_t arr_start = json.find('[', frames_pos);
        if (arr_start == std::string::npos) return false;
        size_t depth = 0;
        size_t obj_start = 0;
        bool in_obj = false;
        std::vector<std::string> frame_blobs;
        for (size_t i = arr_start + 1; i < json.size(); ++i) {
            char c = json[i];
            if (c == '{') {
                if (depth == 0) { obj_start = i; in_obj = true; }
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0 && in_obj) {
                    frame_blobs.push_back(json.substr(obj_start, i - obj_start + 1));
                    in_obj = false;
                }
            } else if (c == ']' && depth == 0) {
                break;
            }
        }
        frames_.reserve(frame_blobs.size());
        for (const auto& blob : frame_blobs) {
            Spectral::SpectralFrame f;
            auto fget = [&](const std::string& k) -> std::string {
                const std::string kk = "\"" + k + "\"";
                size_t p = blob.find(kk);
                if (p == std::string::npos) return {};
                p = blob.find(':', p);
                if (p == std::string::npos) return {};
                ++p;
                while (p < blob.size() && std::isspace(static_cast<unsigned char>(blob[p]))) ++p;
                return blob.substr(p);
            };
            f.frame_index = static_cast<int>(extract_number(fget("frame_index")));
            f.n_fft = static_cast<int>(extract_number(fget("n_fft")));
            f.window_factor = static_cast<float>(extract_number(fget("window_factor")));
            f.timestamp = extract_number(fget("timestamp"));
            f.rms = static_cast<float>(extract_number(fget("rms")));
            f.peak_magnitude = static_cast<float>(extract_number(fget("peak_magnitude")));
            f.spectral_centroid = static_cast<float>(extract_number(fget("spectral_centroid")));
            f.spectral_bandwidth = static_cast<float>(extract_number(fget("spectral_bandwidth")));
            f.magnitudes = extract_array_floats(fget("magnitudes"));
            f.phases = extract_array_floats(fget("phases"));
            f.power = extract_array_floats(fget("power"));
            f.reassigned_times = extract_array_floats(fget("reassigned_times"));
            f.reassigned_freqs = extract_array_floats(fget("reassigned_freqs"));
            frames_.push_back(std::move(f));
        }
    }
    return true;
}

// ============================================================================
// File I/O
// ============================================================================

bool SpectralDataset::save_to_file(const std::string& path, SerializationFormat fmt) const {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    if (fmt == SerializationFormat::Binary) {
        std::vector<uint8_t> buf;
        if (!serialize_binary(buf)) return false;
        ofs.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
    } else {
        std::string s = serialize_json(true);
        ofs.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
    return ofs.good();
}

bool SpectralDataset::load_from_file(const std::string& path, SerializationFormat fmt) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    if (fmt == SerializationFormat::Binary) {
        ifs.seekg(0, std::ios::end);
        const std::streamsize sz = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        if (sz <= 0) return false;
        std::vector<uint8_t> buf(static_cast<size_t>(sz));
        if (!ifs.read(reinterpret_cast<char*>(buf.data()), sz)) return false;
        return deserialize_binary(buf.data(), buf.size());
    } else {
        std::ostringstream os;
        os << ifs.rdbuf();
        return deserialize_json(os.str());
    }
}

// ============================================================================
// Equality
// ============================================================================

bool SpectralDataset::operator==(const SpectralDataset& other) const {
    return version_ == other.version_ &&
           frames_ == other.frames_ &&
           source_meta_ == other.source_meta_ &&
           analysis_meta_ == other.analysis_meta_ &&
           normalization_ == other.normalization_ &&
           freq_axis_ == other.freq_axis_ &&
           time_axis_ == other.time_axis_ &&
           channel_info_ == other.channel_info_ &&
           representation_ == other.representation_;
}

} // namespace Spectral
