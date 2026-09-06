#include "project_config.h"
#include "spectral_dataset.h"      // AnalysisMetadata
#include "spectrogram_renderer.h"  // SpectrogramConfig
#include "spectrum_renderer.h"     // SpectrumConfig

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
#include <map>
#include <sstream>
#include <string>

namespace Spectral {

// ============================================================================
// Helpers — small dependency-free JSON encoder/decoder (canonical form).
//
// Canonical form rules:
//   - Sorted keys at every object level (stable across runs/platforms).
//   - No insignificant whitespace.
//   - Numbers formatted via %.17g for full round-trip of float64.
//   - Strings are UTF-8 (we only emit ASCII); escaped as needed.
// ============================================================================

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string fmt_float(float f) {
    if (std::isnan(f)) return "null";        // JSON has no NaN; we emit null
    if (std::isinf(f)) return f > 0 ? "1e999" : "-1e999";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(f));
    return std::string(buf);
}

std::string fmt_double(double d) {
    if (std::isnan(d)) return "null";
    if (std::isinf(d)) return d > 0 ? "1e999" : "-1e999";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    return std::string(buf);
}

std::string fmt_int(int64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return std::string(buf);
}

std::string fmt_uint(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    return std::string(buf);
}

// Indentation helper for pretty printing.
void put_indent(std::string& out, int n) { for (int i = 0; i < n; ++i) out.push_back(' '); }

// Emit an object's keys in sorted order, using a callback that produces
// "key":value strings when given a key name.
template <typename EmitValue>
void emit_object_sorted(std::string& out, int indent, bool pretty,
                        const std::vector<std::pair<std::string, std::string>>& kvs,
                        bool& first) {
    // kvs must already be sorted by key
    for (const auto& kv : kvs) {
        if (pretty) {
            out.push_back(',');
            out.push_back('\n');
            put_indent(out, indent);
        } else if (!first) {
            out.push_back(',');
        }
        first = false;
        out += json_escape(kv.first);
        out.push_back(':');
        if (pretty) out.push_back(' ');
        out += kv.second;
    }
}

// ============================================================================
// Minimal JSON parser (whitelisted tokens; strict mode rejects unknown fields).
// ============================================================================
class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s), i_(0) {}

    bool skip_ws() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
            else break;
        }
        return i_ < s_.size();
    }
    bool eof() const { return i_ >= s_.size(); }
    char peek() { skip_ws(); return i_ < s_.size() ? s_[i_] : '\0'; }
    bool consume(char c) {
        skip_ws();
        if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; }
        return false;
    }
    bool parse_string(std::string& out) {
        skip_ws();
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') { return true; }
            if (c == '\\') {
                if (i_ >= s_.size()) return false;
                char e = s_[i_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (i_ + 4 > s_.size()) return false;
                        unsigned int code = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s_[i_++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                            else return false;
                        }
                        // emit as 1-3 byte UTF-8 (BMP only)
                        if (code < 0x80) out.push_back(static_cast<char>(code));
                        else if (code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }
    bool parse_number(std::string& out) {
        skip_ws();
        size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_]))
                                  || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'
                                  || s_[i_] == '+' || s_[i_] == '-')) ++i_;
        out = s_.substr(start, i_ - start);
        return !out.empty();
    }
    bool parse_literal(const std::string& lit) {
        skip_ws();
        if (i_ + lit.size() > s_.size()) return false;
        if (s_.compare(i_, lit.size(), lit) != 0) return false;
        i_ += lit.size();
        return true;
    }
    bool parse_null_check() {
        // Returns true if literal `null` was consumed.
        return parse_literal("null");
    }

    const std::string& src() const { return s_; }
    size_t pos() const { return i_; }

private:
    const std::string& s_;
    size_t i_;
};

// Parse a top-level JSON value into a generic representation. We only need
// objects with string keys and string/number/null/bool values. To keep
// implementation tight we use a flat vector of (path, kind, value) entries
// built by a recursive walk.
enum class JKind { Null, Bool, Int, UInt, Float, String };

struct JEntry {
    std::string path;      // e.g. "analysis.window_type"
    JKind kind;
    int64_t i = 0;
    uint64_t u = 0;
    double d = 0.0;
    std::string s;
    bool b = false;
};

bool parse_value(JsonParser& p, std::vector<JEntry>& out, const std::string& base);
bool parse_object(JsonParser& p, std::vector<JEntry>& out, const std::string& base) {
    if (!p.consume('{')) return false;
    bool first = true;
    while (true) {
        if (p.peek() == '}') { p.consume('}'); return true; }
        if (!first && !p.consume(',')) return false;
        first = false;
        std::string k;
        if (!p.parse_string(k)) return false;
        if (!p.consume(':')) return false;
        if (!parse_value(p, out, base.empty() ? k : base + "." + k)) return false;
    }
}

bool parse_array(JsonParser& p, std::vector<JEntry>& out, const std::string& base) {
    if (!p.consume('[')) return false;
    int idx = 0;
    bool first = true;
    while (true) {
        if (p.peek() == ']') { p.consume(']'); return true; }
        if (!first && !p.consume(',')) return false;
        first = false;
        std::string child = base + "[" + std::to_string(idx++) + "]";
        if (!parse_value(p, out, child)) return false;
    }
}

bool parse_value(JsonParser& p, std::vector<JEntry>& out, const std::string& base) {
    char c = p.peek();
    if (c == '{') return parse_object(p, out, base);
    if (c == '[') return parse_array(p, out, base);
    if (c == '"') {
        std::string s;
        if (!p.parse_string(s)) return false;
        JEntry e; e.path = base; e.kind = JKind::String; e.s = std::move(s);
        out.push_back(std::move(e));
        return true;
    }
    if (c == 't' || c == 'f') {
        bool tt = (c == 't');
        if (!p.parse_literal(tt ? "true" : "false")) return false;
        JEntry e; e.path = base; e.kind = JKind::Bool; e.b = tt;
        out.push_back(std::move(e));
        return true;
    }
    if (c == 'n') {
        if (!p.parse_null_check()) return false;
        JEntry e; e.path = base; e.kind = JKind::Null;
        out.push_back(std::move(e));
        return true;
    }
    // number
    std::string num;
    if (!p.parse_number(num)) return false;
    JEntry e; e.path = base;
    if (num.find('.') != std::string::npos ||
        num.find('e') != std::string::npos || num.find('E') != std::string::npos) {
        e.kind = JKind::Float;
        e.d = std::strtod(num.c_str(), nullptr);
    } else {
        e.kind = JKind::Int;
        e.i = std::strtoll(num.c_str(), nullptr, 10);
    }
    out.push_back(std::move(e));
    return true;
}

// Look up a key with exact path equality.
const JEntry* find(const std::vector<JEntry>& v, const std::string& p) {
    for (const auto& e : v) if (e.path == p) return &e;
    return nullptr;
}

// Returns true if `prefix` matches the start of `path` (followed by '.' or '[').
bool has_prefix(const std::string& path, const std::string& prefix) {
    if (path.size() < prefix.size()) return false;
    if (path.compare(0, prefix.size(), prefix) != 0) return false;
    if (path.size() == prefix.size()) return true;
    char c = path[prefix.size()];
    return c == '.' || c == '[';
}

// SHA-256 of a byte buffer (tiny self-contained impl, no deps).
struct Sha256 {
    uint32_t h[8];
    uint64_t bitlen = 0;
    uint8_t buf[64];
    size_t buflen = 0;
    static constexpr uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    static inline uint32_t rotr(uint32_t x, uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }
    Sha256() {
        h[0]=0x6a09e667; h[1]=0xbb67ae85; h[2]=0x3c6ef372; h[3]=0xa54ff53a;
        h[4]=0x510e527f; h[5]=0x9b05688c; h[6]=0x1f83d9ab; h[7]=0x5be0cd19;
    }
    void compress() {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(buf[4*i])<<24) | (uint32_t(buf[4*i+1])<<16) |
                   (uint32_t(buf[4*i+2])<<8) | uint32_t(buf[4*i+3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    void update(const uint8_t* data, size_t n) {
        bitlen += n * 8;
        while (n) {
            size_t take = std::min(n, sizeof(buf) - buflen);
            std::memcpy(buf + buflen, data, take);
            buflen += take; data += take; n -= take;
            if (buflen == 64) { compress(); buflen = 0; }
        }
    }
    std::string finish() {
        buf[buflen++] = 0x80;
        if (buflen > 56) { while (buflen < 64) buf[buflen++] = 0; compress(); buflen = 0; }
        while (buflen < 56) buf[buflen++] = 0;
        for (int i = 7; i >= 0; --i) buf[buflen++] = (uint8_t)(bitlen >> (i*8));
        compress();
        char hex[65];
        for (int i = 0; i < 8; ++i) {
            std::snprintf(hex + i*8, 9, "%08x", h[i]);
        }
        hex[64] = 0;
        return std::string(hex, 64);
    }
    static std::string hash(const std::string& s) {
        Sha256 h;
        h.update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        return h.finish();
    }
};

} // namespace

// ============================================================================
// Enum helpers
// ============================================================================

const char* window_type_name(ProjectWindowType w) {
    switch (w) {
        case ProjectWindowType::Rectangular: return "rectangular";
        case ProjectWindowType::Hann:        return "hann";
        case ProjectWindowType::Hamming:     return "hamming";
        case ProjectWindowType::Blackman:    return "blackman";
    }
    return "hann";
}
bool try_parse_window_type(const std::string& s, ProjectWindowType& out) {
    if (s == "rectangular") { out = ProjectWindowType::Rectangular; return true; }
    if (s == "hamming")     { out = ProjectWindowType::Hamming;     return true; }
    if (s == "blackman")    { out = ProjectWindowType::Blackman;    return true; }
    if (s == "hann")        { out = ProjectWindowType::Hann;        return true; }
    return false;
}
ProjectWindowType parse_window_type(const std::string& s) {
    ProjectWindowType w;
    return try_parse_window_type(s, w) ? w : ProjectWindowType::Hann;
}
const char* renderer_kind_name(RendererKind k) {
    return k == RendererKind::Spectrogram ? "spectrogram" : "spectrum";
}
bool try_parse_renderer_kind(const std::string& s, RendererKind& out) {
    if (s == "spectrogram") { out = RendererKind::Spectrogram; return true; }
    if (s == "spectrum")    { out = RendererKind::Spectrum;    return true; }
    return false;
}
RendererKind parse_renderer_kind(const std::string& s) {
    RendererKind k;
    return try_parse_renderer_kind(s, k) ? k : RendererKind::Spectrum;
}
const char* freq_scale_name(ProjectFreqScale s) {
    switch (s) {
        case ProjectFreqScale::Linear:      return "linear";
        case ProjectFreqScale::Logarithmic: return "logarithmic";
        case ProjectFreqScale::Mel:         return "mel";
        case ProjectFreqScale::Bark:        return "bark";
        case ProjectFreqScale::Erb:         return "erb";
        case ProjectFreqScale::Cqt:         return "cqt";
    }
    return "logarithmic";
}
bool try_parse_freq_scale(const std::string& s, ProjectFreqScale& out) {
    if (s == "linear")      { out = ProjectFreqScale::Linear;      return true; }
    if (s == "logarithmic") { out = ProjectFreqScale::Logarithmic; return true; }
    if (s == "mel")         { out = ProjectFreqScale::Mel;         return true; }
    if (s == "bark")        { out = ProjectFreqScale::Bark;        return true; }
    if (s == "erb")         { out = ProjectFreqScale::Erb;         return true; }
    if (s == "cqt")         { out = ProjectFreqScale::Cqt;         return true; }
    return false;
}
ProjectFreqScale parse_freq_scale(const std::string& s) {
    ProjectFreqScale f;
    return try_parse_freq_scale(s, f) ? f : ProjectFreqScale::Logarithmic;
}
const char* color_map_name(ProjectColorMap m) {
    return m == ProjectColorMap::Heat ? "heat" : "viridis";
}
bool try_parse_color_map(const std::string& s, ProjectColorMap& out) {
    if (s == "heat")    { out = ProjectColorMap::Heat;    return true; }
    if (s == "viridis") { out = ProjectColorMap::Viridis; return true; }
    return false;
}
ProjectColorMap parse_color_map(const std::string& s) {
    ProjectColorMap m;
    return try_parse_color_map(s, m) ? m : ProjectColorMap::Viridis;
}
bool try_parse_reproducibility_tier(const std::string& s, ReproducibilityTier& out) {
    if (s == "spectral_image_not_video") { out = ReproducibilityTier::Spectral_Image_NotVideo; return true; }
    if (s == "none") { out = ReproducibilityTier::None; return true; }
    return false;
}
const char* reproducibility_tier_name(ReproducibilityTier t) {
    switch (t) {
        case ReproducibilityTier::Spectral_Image_NotVideo: return "spectral_image_not_video";
        case ReproducibilityTier::None: return "none";
    }
    return "none";
}
ProjectFreqScale to_renderer_freq_scale(ProjectFreqScale s) { return s; }
ProjectColorMap to_renderer_color_map(ProjectColorMap m) { return m; }

// ============================================================================
// Validation
// ============================================================================
bool ProjectConfig::validate(std::vector<std::string>& errors) const {
    errors.clear();
    if (schema_version < PROJECT_CONFIG_MIN_COMPATIBLE_VERSION) {
        errors.push_back("schema_version is older than the minimum compatible version");
    }
    if (schema_version > PROJECT_CONFIG_SCHEMA_VERSION) {
        errors.push_back("schema_version is newer than this software supports");
    }
    // Canonical configs carry no defaults: fft/hop follow the same contract
    // as GenerateConfig (pow2 >= 2, hop in (0, fft]).
    if (analysis.fft_size < 2 || (analysis.fft_size & (analysis.fft_size - 1)) != 0) {
        errors.push_back("analysis.fft_size must be a power of two >= 2");
    }
    if (analysis.hop_size <= 0 || analysis.hop_size > analysis.fft_size) {
        errors.push_back("analysis.hop_size must be in (0, fft_size]");
    }
    if (analysis.window_type != ProjectWindowType::Rectangular &&
        analysis.window_type != ProjectWindowType::Hann &&
        analysis.window_type != ProjectWindowType::Hamming &&
        analysis.window_type != ProjectWindowType::Blackman) {
        errors.push_back("analysis.window_type is not a known window");
    }
    if (analysis.analysis_method.empty()) {
        errors.push_back("analysis.analysis_method must be non-empty");
    }
    if (analysis.analysis_version == 0) {
        errors.push_back("analysis.analysis_version must be > 0");
    }
    if (analysis.analyzed_channels < 0 || analysis.channel_mapping < 0) {
        errors.push_back("analysis channel counts must be >= 0");
    }
    if (input.num_channels < 0) {
        errors.push_back("input.num_channels must be >= 0");
    }
    if (reproducibility_tier != ReproducibilityTier::Spectral_Image_NotVideo &&
        reproducibility_tier != ReproducibilityTier::None) {
        errors.push_back("reproducibility_tier is not a known tier");
    }
    if (analysis.overlap_ratio < 0.0f || analysis.overlap_ratio >= 1.0f) {
        errors.push_back("analysis.overlap_ratio must be in [0, 1)");
    }
    if (analysis.sample_rate <= 0) {
        errors.push_back("analysis.sample_rate must be positive");
    }
    if (input.sample_rate <= 0) {
        errors.push_back("input.sample_rate must be positive");
    }
    if (dynamic_range.db_floor >= dynamic_range.db_ceiling) {
        errors.push_back("dynamic_range.db_floor must be < db_ceiling");
    }
    if (frequency_range.min_hz < 0.0f) {
        errors.push_back("frequency_range.min_hz must be >= 0");
    }
    if (frequency_range.max_hz < 0.0f) {
        errors.push_back("frequency_range.max_hz must be >= 0");
    }
    if (frequency_range.max_hz > 0.0f && frequency_range.min_hz >= frequency_range.max_hz) {
        errors.push_back("frequency_range.min_hz must be below max_hz");
    }
    if (renderer.width <= 0 || renderer.height <= 0) {
        errors.push_back("renderer.width/height must be positive");
    }
    if (renderer.line_thickness <= 0) {
        errors.push_back("renderer.line_thickness must be positive");
    }
    if (renderer.grid_divisions_x <= 0 || renderer.grid_divisions_y <= 0) {
        errors.push_back("renderer.grid_divisions must be positive");
    }
    return errors.empty();
}

std::string ProjectConfig::fingerprint() const {
    return Sha256::hash(ProjectConfigSerializer::to_json(*this));
}

std::string sha256_hex(const uint8_t* data, size_t size) {
    Sha256 h;
    h.update(data, size);
    return h.finish();
}

std::string sha256_hex(const std::string& s) {
    return Sha256::hash(s);
}

bool sha256_file(const std::string& path, std::string& out_hex, std::string& error) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) {
        error = "cannot open " + path + " for hashing";
        return false;
    }
    Sha256 h;
    std::vector<uint8_t> buf(1 << 20);
    while (f.good()) {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        if (n > 0) h.update(buf.data(), static_cast<size_t>(n));
    }
    if (f.bad()) {
        error = "read failed while hashing " + path;
        return false;
    }
    out_hex = h.finish();
    return true;
}

// ============================================================================
// JSON serialization
// ============================================================================
namespace {

// Build a sorted key/value list for a given struct. We do this by writing
// the JSON manually for each ProjectConfig substruct (the struct is small
// and fields are well known).
using KV = std::pair<std::string, std::string>;

void emit_input(const ProjectInput& in, std::vector<KV>& kvs) {
    kvs.push_back({"codec_long_name",     json_escape(in.codec_long_name)});
    kvs.push_back({"codec_name",          json_escape(in.codec_name)});
    kvs.push_back({"duration_seconds",    fmt_double(in.duration_seconds)});
    kvs.push_back({"file_hash",           json_escape(in.file_hash)});
    kvs.push_back({"file_path",           json_escape(in.file_path)});
    kvs.push_back({"file_size_bytes",     fmt_uint(in.file_size_bytes)});
    kvs.push_back({"num_channels",        fmt_int(in.num_channels)});
    kvs.push_back({"sample_rate",         fmt_int(in.sample_rate)});
}
void emit_analysis(const ProjectAnalysis& a, std::vector<KV>& kvs) {
    kvs.push_back({"analysis_method",     json_escape(a.analysis_method)});
    kvs.push_back({"analysis_version",    fmt_uint(a.analysis_version)});
    kvs.push_back({"analyzed_channels",   fmt_int(a.analyzed_channels)});
    kvs.push_back({"channel_mapping",     fmt_int(a.channel_mapping)});
    kvs.push_back({"fft_size",            fmt_int(a.fft_size)});
    kvs.push_back({"hop_size",            fmt_int(a.hop_size)});
    kvs.push_back({"magnitude_scale",     fmt_float(a.magnitude_scale)});
    kvs.push_back({"overlap_ratio",       fmt_float(a.overlap_ratio)});
    kvs.push_back({"phase_unwrap",        fmt_float(a.phase_unwrap)});
    kvs.push_back({"sample_rate",         fmt_int(a.sample_rate)});
    kvs.push_back({"window_coherent_gain",fmt_float(a.window_coherent_gain)});
    kvs.push_back({"window_type",         json_escape(window_type_name(a.window_type))});
}
void emit_dynamic_range(const ProjectDynamicRange& d, std::vector<KV>& kvs) {
    kvs.push_back({"db_ceiling",          fmt_float(d.db_ceiling)});
    kvs.push_back({"db_floor",            fmt_float(d.db_floor)});
    kvs.push_back({"reference_amplitude", fmt_float(d.reference_amplitude)});
    kvs.push_back({"window_energy_gain",  fmt_float(d.window_energy_gain)});
}
void emit_frequency_range(const ProjectFrequencyRange& f, std::vector<KV>& kvs) {
    kvs.push_back({"max_hz",  fmt_float(f.max_hz)});
    kvs.push_back({"min_hz",  fmt_float(f.min_hz)});
    kvs.push_back({"scale",   json_escape(freq_scale_name(f.scale))});
}
void emit_renderer(const ProjectRenderer& r, std::vector<KV>& kvs) {
    kvs.push_back({"color_map",       json_escape(color_map_name(r.color_map))});
    kvs.push_back({"cqt_center_hz",     fmt_float(r.cqt_center_hz)});
    kvs.push_back({"cqt_q",             fmt_float(r.cqt_q)});
    kvs.push_back({"dpi",             fmt_int(r.dpi)});
    kvs.push_back({"draw_grid",       r.draw_grid ? "true" : "false"});
    kvs.push_back({"draw_labels",     r.draw_labels ? "true" : "false"});
    kvs.push_back({"grid_divisions_x",fmt_int(r.grid_divisions_x)});
    kvs.push_back({"grid_divisions_y",fmt_int(r.grid_divisions_y)});
    kvs.push_back({"height",          fmt_int(r.height)});
    kvs.push_back({"kind",            json_escape(renderer_kind_name(r.kind))});
    kvs.push_back({"line_thickness",  fmt_int(r.line_thickness)});
    kvs.push_back({"width",           fmt_int(r.width)});
}

void emit_object(std::string& out, int indent, bool pretty,
                 const std::vector<KV>& kvs) {
    // kvs is already sorted by caller
    out.push_back('{');
    if (pretty) out.push_back('\n');
    bool first = true;
    for (const auto& kv : kvs) {
        if (pretty) {
            if (!first) { out.push_back(','); out.push_back('\n'); }
            put_indent(out, indent + 2);
        } else if (!first) {
            out.push_back(',');
        }
        first = false;
        out += json_escape(kv.first);
        out.push_back(':');
        if (pretty) out.push_back(' ');
        out += kv.second;
    }
    if (pretty) { out.push_back('\n'); put_indent(out, indent); }
    out.push_back('}');
}

} // namespace

std::string ProjectConfigSerializer::to_json(const ProjectConfig& cfg) {
    std::vector<KV> top;
    auto add_obj = [&](const std::string& key, const std::vector<KV>& sub) {
        std::string inner;
        emit_object(inner, 0, false, sub);
        top.push_back({key, std::move(inner)});
    };
    { std::vector<KV> v; emit_input(cfg.input, v);            add_obj("input", v); }
    { std::vector<KV> v; emit_analysis(cfg.analysis, v);        add_obj("analysis", v); }
    { std::vector<KV> v; emit_dynamic_range(cfg.dynamic_range, v); add_obj("dynamic_range", v); }
    { std::vector<KV> v; emit_frequency_range(cfg.frequency_range, v); add_obj("frequency_range", v); }
    { std::vector<KV> v; emit_renderer(cfg.renderer, v);        add_obj("renderer", v); }
    top.push_back({"created_utc",  json_escape(cfg.created_utc)});
    top.push_back({"notes",        json_escape(cfg.notes)});
    top.push_back({"project_id",   json_escape(cfg.project_id)});
    top.push_back({"reproducibility_tier", json_escape(reproducibility_tier_name(cfg.reproducibility_tier))});
    top.push_back({"schema_version",  fmt_uint(cfg.schema_version)});
    top.push_back({"software_name",   json_escape(cfg.software_name)});
    top.push_back({"software_version",json_escape(cfg.software_version)});
    std::sort(top.begin(), top.end(),
              [](const KV& a, const KV& b) { return a.first < b.first; });
    std::string out;
    emit_object(out, 0, false, top);
    return out;
}

std::string ProjectConfigSerializer::to_json_pretty(const ProjectConfig& cfg, int indent) {
    std::vector<KV> top;
    auto add_obj = [&](const std::string& key, const std::vector<KV>& sub) {
        std::string inner;
        emit_object(inner, indent, true, sub);
        top.push_back({key, std::move(inner)});
    };
    { std::vector<KV> v; emit_input(cfg.input, v);            add_obj("input", v); }
    { std::vector<KV> v; emit_analysis(cfg.analysis, v);        add_obj("analysis", v); }
    { std::vector<KV> v; emit_dynamic_range(cfg.dynamic_range, v); add_obj("dynamic_range", v); }
    { std::vector<KV> v; emit_frequency_range(cfg.frequency_range, v); add_obj("frequency_range", v); }
    { std::vector<KV> v; emit_renderer(cfg.renderer, v);        add_obj("renderer", v); }
    top.push_back({"created_utc",  json_escape(cfg.created_utc)});
    top.push_back({"notes",        json_escape(cfg.notes)});
    top.push_back({"project_id",   json_escape(cfg.project_id)});
    top.push_back({"reproducibility_tier", json_escape(reproducibility_tier_name(cfg.reproducibility_tier))});
    top.push_back({"schema_version",  fmt_uint(cfg.schema_version)});
    top.push_back({"software_name",   json_escape(cfg.software_name)});
    top.push_back({"software_version",json_escape(cfg.software_version)});
    std::sort(top.begin(), top.end(),
              [](const KV& a, const KV& b) { return a.first < b.first; });
    std::string out;
    emit_object(out, 0, true, top);
    return out;
}

bool ProjectConfigSerializer::from_json(const std::string& json, ProjectConfig& out,
                                        std::string& error, bool allow_unknown) {
    out = ProjectConfig{};
    JsonParser p(json);
    std::vector<JEntry> entries;
    if (!parse_object(p, entries, "")) {
        error = "failed to parse top-level JSON object";
        return false;
    }
    p.skip_ws();
    if (!p.eof()) {
        error = "trailing content after top-level object";
        return false;
    }
    // Helper: get entry by path; if missing, return false but no error.
    auto get = [&](const std::string& path, const JEntry*& e) -> bool {
        e = find(entries, path);
        return e != nullptr;
    };
    // Type check
    auto expect_str = [&](const JEntry* e, const std::string& path) -> std::string {
        if (!e || e->kind != JKind::String) return "";
        return e->s;
    };
    auto expect_f = [&](const JEntry* e) -> float {
        if (!e) return 0.0f;
        if (e->kind == JKind::Float) return static_cast<float>(e->d);
        if (e->kind == JKind::Int)   return static_cast<float>(e->i);
        if (e->kind == JKind::UInt)  return static_cast<float>(e->u);
        if (e->kind == JKind::Bool)  return e->b ? 1.0f : 0.0f;
        return 0.0f;
    };
    auto expect_d = [&](const JEntry* e) -> double {
        if (!e) return 0.0;
        if (e->kind == JKind::Float) return e->d;
        if (e->kind == JKind::Int)   return static_cast<double>(e->i);
        if (e->kind == JKind::UInt)  return static_cast<double>(e->u);
        return 0.0;
    };
    auto expect_i = [&](const JEntry* e) -> int {
        if (!e) return 0;
        if (e->kind == JKind::Int)   return static_cast<int>(e->i);
        if (e->kind == JKind::UInt)  return static_cast<int>(e->u);
        if (e->kind == JKind::Float) return static_cast<int>(e->d);
        if (e->kind == JKind::Bool)  return e->b ? 1 : 0;
        return 0;
    };
    auto expect_u64 = [&](const JEntry* e) -> uint64_t {
        if (!e) return 0;
        if (e->kind == JKind::UInt)  return e->u;
        if (e->kind == JKind::Int)   return static_cast<uint64_t>(e->i);
        if (e->kind == JKind::Float) return static_cast<uint64_t>(e->d);
        return 0;
    };
    auto expect_b = [&](const JEntry* e) -> bool {
        if (!e) return false;
        if (e->kind == JKind::Bool)  return e->b;
        if (e->kind == JKind::Int)   return e->i != 0;
        if (e->kind == JKind::UInt)  return e->u != 0;
        if (e->kind == JKind::Float) return e->d != 0.0;
        return false;
    };

    // input.*
    {
        const JEntry* e;
        if (get("input.codec_long_name", e))     out.input.codec_long_name = expect_str(e, "input.codec_long_name");
        if (get("input.codec_name", e))          out.input.codec_name = expect_str(e, "input.codec_name");
        if (get("input.duration_seconds", e))    out.input.duration_seconds = expect_d(e);
        if (get("input.file_hash", e))           out.input.file_hash = expect_str(e, "input.file_hash");
        if (get("input.file_path", e))           out.input.file_path = expect_str(e, "input.file_path");
        if (get("input.file_size_bytes", e))     out.input.file_size_bytes = expect_u64(e);
        if (get("input.num_channels", e))        out.input.num_channels = expect_i(e);
        if (get("input.sample_rate", e))         out.input.sample_rate = expect_i(e);
    }
    // analysis.*
    {
        const JEntry* e;
        if (get("analysis.analyzed_channels", e))   out.analysis.analyzed_channels = expect_i(e);
        if (get("analysis.channel_mapping", e))     out.analysis.channel_mapping = expect_i(e);
        if (get("analysis.fft_size", e))            out.analysis.fft_size = expect_i(e);
        if (get("analysis.hop_size", e))            out.analysis.hop_size = expect_i(e);
        if (get("analysis.magnitude_scale", e))     out.analysis.magnitude_scale = expect_f(e);
        if (get("analysis.overlap_ratio", e))       out.analysis.overlap_ratio = expect_f(e);
        if (get("analysis.phase_unwrap", e))        out.analysis.phase_unwrap = expect_f(e);
        if (get("analysis.sample_rate", e))         out.analysis.sample_rate = expect_i(e);
        if (get("analysis.window_coherent_gain",e)) out.analysis.window_coherent_gain = expect_f(e);
        if (get("analysis.window_type", e)) {
            ProjectWindowType w;
            if (!try_parse_window_type(expect_str(e, "analysis.window_type"), w)) {
                error = "unknown analysis.window_type";
                return false;
            }
            out.analysis.window_type = w;
        }
        if (get("analysis.analysis_method", e))  out.analysis.analysis_method = expect_str(e, "analysis.analysis_method");
        if (get("analysis.analysis_version", e)) out.analysis.analysis_version = static_cast<uint32_t>(expect_u64(e));
    }
    // dynamic_range.*
    {
        const JEntry* e;
        if (get("dynamic_range.db_ceiling", e))          out.dynamic_range.db_ceiling = expect_f(e);
        if (get("dynamic_range.db_floor", e))            out.dynamic_range.db_floor = expect_f(e);
        if (get("dynamic_range.reference_amplitude", e)) out.dynamic_range.reference_amplitude = expect_f(e);
        if (get("dynamic_range.window_energy_gain", e))  out.dynamic_range.window_energy_gain = expect_f(e);
    }
    // frequency_range.*
    {
        const JEntry* e;
        if (get("frequency_range.max_hz", e))  out.frequency_range.max_hz = expect_f(e);
        if (get("frequency_range.min_hz", e))  out.frequency_range.min_hz = expect_f(e);
        if (get("frequency_range.scale", e)) {
            ProjectFreqScale s;
            if (!try_parse_freq_scale(expect_str(e, "frequency_range.scale"), s)) {
                error = "unknown frequency_range.scale";
                return false;
            }
            out.frequency_range.scale = s;
        }
    }
    // renderer.*
    {
        const JEntry* e;
        if (get("renderer.cqt_center_hz", e)) out.renderer.cqt_center_hz = expect_f(e);
        if (get("renderer.cqt_q", e))         out.renderer.cqt_q = expect_f(e);
        if (get("renderer.color_map", e)) {
            ProjectColorMap m;
            if (!try_parse_color_map(expect_str(e, "renderer.color_map"), m)) {
                error = "unknown renderer.color_map";
                return false;
            }
            out.renderer.color_map = m;
        }
        if (get("renderer.dpi", e))               out.renderer.dpi = expect_i(e);
        if (get("renderer.draw_grid", e))         out.renderer.draw_grid = expect_b(e);
        if (get("renderer.draw_labels", e))       out.renderer.draw_labels = expect_b(e);
        if (get("renderer.grid_divisions_x", e))  out.renderer.grid_divisions_x = expect_i(e);
        if (get("renderer.grid_divisions_y", e))  out.renderer.grid_divisions_y = expect_i(e);
        if (get("renderer.height", e))            out.renderer.height = expect_i(e);
        if (get("renderer.kind", e)) {
            RendererKind k;
            if (!try_parse_renderer_kind(expect_str(e, "renderer.kind"), k)) {
                error = "unknown renderer.kind";
                return false;
            }
            out.renderer.kind = k;
        }
        if (get("renderer.line_thickness", e))    out.renderer.line_thickness = expect_i(e);
        if (get("renderer.width", e))             out.renderer.width = expect_i(e);
    }
    // top-level
    {
        const JEntry* e;
        if (get("created_utc", e))  out.created_utc = expect_str(e, "created_utc");
        if (get("notes", e))        out.notes = expect_str(e, "notes");
        if (get("project_id", e))   out.project_id = expect_str(e, "project_id");
        if (get("reproducibility_tier", e)) {
            ReproducibilityTier t;
            if (!try_parse_reproducibility_tier(expect_str(e, "reproducibility_tier"), t)) {
                error = "unknown reproducibility_tier";
                return false;
            }
            out.reproducibility_tier = t;
        }
        if (get("schema_version", e))   out.schema_version = static_cast<uint32_t>(expect_u64(e));
        if (get("software_name", e))    out.software_name = expect_str(e, "software_name");
        if (get("software_version", e)) out.software_version = expect_str(e, "software_version");
    }
    if (out.schema_version < PROJECT_CONFIG_MIN_COMPATIBLE_VERSION) {
        error = "schema_version " + std::to_string(out.schema_version) +
                " is older than minimum " + std::to_string(PROJECT_CONFIG_MIN_COMPATIBLE_VERSION);
        return false;
    }
    // Strict mode: reject unknown top-level and known subobject fields.
    if (!allow_unknown) {
        std::vector<std::string> known_top = {
            "analysis", "created_utc", "dynamic_range", "frequency_range",
            "input", "notes", "project_id", "renderer", "reproducibility_tier",
            "schema_version", "software_name", "software_version"
        };
        for (const auto& e : entries) {
            // top-level keys have no '.' in path
            if (e.path.find('.') != std::string::npos) continue;
            bool ok = false;
            for (const auto& k : known_top) if (k == e.path) { ok = true; break; }
            if (!ok) { error = "unknown top-level field: " + e.path; return false; }
        }
    }
    return true;
}

bool ProjectConfigSerializer::save(const std::string& path, const ProjectConfig& cfg,
                                   std::string& error, bool pretty) {
    std::ofstream f;
    f.open(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f.is_open()) { error = "cannot open " + path + " for writing"; return false; }
    std::string j = pretty ? to_json_pretty(cfg) : to_json(cfg);
    f.write(j.data(), static_cast<std::streamsize>(j.size()));
    if (!f.good()) { error = "write failed"; return false; }
    f.close();
    return true;
}
bool ProjectConfigSerializer::load(const std::string& path, ProjectConfig& out,
                                   std::string& error, bool allow_unknown) {
    std::ifstream f;
    f.open(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) { error = "cannot open " + path + " for reading"; return false; }
    std::ostringstream ss; ss << f.rdbuf();
    f.close();
    return from_json(ss.str(), out, error, allow_unknown);
}

// Canonical subset: input content identity + everything that can change
// the analytical result. Informational fields (notes, project_id,
// created_utc, renderer, software_*, schema_version, tier) do not move it.
std::string ProjectConfig::analysis_fingerprint() const {
    std::vector<KV> top;
    {
        std::vector<KV> v;
        v.push_back({"file_hash", json_escape(input.file_hash)});
        v.push_back({"file_size_bytes", fmt_uint(input.file_size_bytes)});
        std::string inner;
        emit_object(inner, 0, false, v);
        top.push_back({"input", std::move(inner)});
    }
    {
        std::vector<KV> v;
        emit_analysis(analysis, v);
        std::string inner;
        emit_object(inner, 0, false, v);
        top.push_back({"analysis", std::move(inner)});
    }
    std::sort(top.begin(), top.end(),
              [](const KV& a, const KV& b) { return a.first < b.first; });
    std::string out;
    emit_object(out, 0, false, top);
    return Sha256::hash(out);
}

std::string ProjectConfig::render_fingerprint(const std::string& dataset_identity) const {
    std::vector<KV> top;
    top.push_back({"dataset", json_escape(dataset_identity)});
    {
        std::vector<KV> v;
        emit_dynamic_range(dynamic_range, v);
        std::string inner;
        emit_object(inner, 0, false, v);
        top.push_back({"dynamic_range", std::move(inner)});
    }
    {
        std::vector<KV> v;
        emit_frequency_range(frequency_range, v);
        std::string inner;
        emit_object(inner, 0, false, v);
        top.push_back({"frequency_range", std::move(inner)});
    }
    {
        std::vector<KV> v;
        emit_renderer(renderer, v);
        std::string inner;
        emit_object(inner, 0, false, v);
        top.push_back({"renderer", std::move(inner)});
    }
    std::sort(top.begin(), top.end(),
              [](const KV& a, const KV& b) { return a.first < b.first; });
    std::string out;
    emit_object(out, 0, false, top);
    return Sha256::hash(out);
}

// ============================================================================
// Adapter
// ============================================================================

void ProjectConfigAdapter::to_analysis_metadata(const ProjectConfig& cfg, AnalysisMetadata& out) {
    out.window_type = window_type_name(cfg.analysis.window_type);
    out.window_coherent_gain = cfg.analysis.window_coherent_gain;
    out.fft_size = cfg.analysis.fft_size;
    out.hop_size = cfg.analysis.hop_size;
    out.overlap_ratio = cfg.analysis.overlap_ratio;
    out.sample_rate = cfg.analysis.sample_rate;
    out.analyzed_channels = cfg.analysis.analyzed_channels;
    out.channel_mapping = cfg.analysis.channel_mapping;
    out.magnitude_scale = cfg.analysis.magnitude_scale;
    out.phase_unwrap = cfg.analysis.phase_unwrap;
    out.nyquist_frequency = cfg.analysis.sample_rate / 2.0f;
    out.num_frequency_bins = cfg.analysis.fft_size / 2 + 1;
    // frame/total fields left at zero — caller fills from data
    out.analyzer_version = cfg.software_version;
}

static FrequencyScale adapt_scale(ProjectFreqScale s) {
    switch (s) {
        case ProjectFreqScale::Linear:      return FrequencyScale::Linear;
        case ProjectFreqScale::Logarithmic: return FrequencyScale::Logarithmic;
        case ProjectFreqScale::Mel:         return FrequencyScale::Mel;
        case ProjectFreqScale::Bark:        return FrequencyScale::Bark;
        case ProjectFreqScale::Erb:         return FrequencyScale::Erb;
        case ProjectFreqScale::Cqt:         return FrequencyScale::CQT;
    }
    return FrequencyScale::Logarithmic;
}

void ProjectConfigAdapter::to_spectrum_config(const ProjectConfig& cfg, SpectrumConfig& out) {
    out.width = cfg.renderer.width;
    out.height = cfg.renderer.height;
    out.freq_scale = adapt_scale(cfg.frequency_range.scale);
    out.freq_min_hz = cfg.frequency_range.min_hz;
    out.freq_max_hz = cfg.frequency_range.max_hz;
    out.y_scale = SpectrumScale::Decibels;
    out.db_floor = cfg.dynamic_range.db_floor;
    out.db_ceiling = cfg.dynamic_range.db_ceiling;
    out.y_max_amplitude = 1.0f;
    out.reference_amplitude = cfg.dynamic_range.reference_amplitude;
    out.aggregation = SpectrumAggregation::Mean;
    out.color_map = (cfg.renderer.color_map == ProjectColorMap::Heat)
                    ? ColorMap::Heat : ColorMap::Viridis;
    out.draw_grid = cfg.renderer.draw_grid;
    out.draw_labels = cfg.renderer.draw_labels;
    out.line_thickness = cfg.renderer.line_thickness;
    out.grid_divisions_x = cfg.renderer.grid_divisions_x;
    out.grid_divisions_y = cfg.renderer.grid_divisions_y;
    out.cqt_center_hz = cfg.renderer.cqt_center_hz;
    out.cqt_q = cfg.renderer.cqt_q;
    out.seed = 0;
}

void ProjectConfigAdapter::to_spectrogram_config(const ProjectConfig& cfg, SpectrogramConfig& out) {
    out.width = cfg.renderer.width;
    out.height = cfg.renderer.height;
    out.color_map = (cfg.renderer.color_map == ProjectColorMap::Heat)
                    ? ColorMap::Heat : ColorMap::Viridis;
    out.freq_scale = adapt_scale(cfg.frequency_range.scale);
    out.interpolation = Interpolation::Bilinear;
    out.db_floor = cfg.dynamic_range.db_floor;
    out.db_ceiling = cfg.dynamic_range.db_ceiling;
    out.freq_min_hz = cfg.frequency_range.min_hz;
    out.freq_max_hz = cfg.frequency_range.max_hz;
    out.cqt_center_hz = cfg.renderer.cqt_center_hz;
    out.cqt_q = cfg.renderer.cqt_q;
    out.seed = 0;
}

} // namespace Spectral
