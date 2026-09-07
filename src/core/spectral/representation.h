#pragma once

// S6.0 — spectral representation model (ARCHITECTURE ONLY, no algorithms).
//
// Representation = WHAT DATA WAS COMPUTED. Rendering scale = HOW IT IS
// DISPLAYED. STFT data on a logarithmic display and Mel data on a linear
// display are different representations, not different scales of one
// thing. Never derive non-STFT bin counts from fft_size/2+1.
//
// Nothing here computes spectra; it only describes them so future
// Mel/Bark/ERB/CQT algorithms have an unambiguous contract to fill.

#include <cstdint>
#include <string>
#include <vector>

namespace Spectral {

// What was computed. Values are stable (persisted + fingerprinted).
enum class RepresentationKind {
    STFT = 0,
    Mel = 1,
    Bark = 2,
    Erb = 3,
    Cqt = 4,
};

// Filterbank energy normalization policy (Mel/Bark/ERB).
enum class RepresentationNorm {
    None = 0,    // raw filterbank energies
    Slaney = 1,  // area-normalized triangular filters (Slaney-style)
    Area = 2,    // unit-area filters
};

// Whether per-bin phase is meaningful for the representation.
// STFT phase is physical; filterbank magnitudes have no FFT phase —
// do not fabricate it.
enum class RepresentationPhase {
    Available = 0,
    NotApplicable = 1,
};

struct RepresentationInfo {
    RepresentationKind kind = RepresentationKind::STFT;
    // Bins produced per frame. 0 = implied by the FFT grid (STFT only:
    // bins == fft_size/2+1). Non-STFT kinds MUST set an explicit count.
    int bins = 0;
    float fmin_hz = 0.0f;  // 0 = implied full range (STFT: DC)
    float fmax_hz = 0.0f;  // 0 = implied full range (STFT: Nyquist)
    // Mel/Bark/ERB: number of filters. CQT: bins per octave.
    int bands = 0;
    // CQT: quality factor (0 = derive from bins-per-octave at build time).
    float q = 0.0f;
    RepresentationNorm norm = RepresentationNorm::None;
    RepresentationPhase phase = RepresentationPhase::Available;
    // Whether reassignment coordinates are meaningful for this data.
    bool reassignment_supported = false;
    // Explicit bin centers in Hz. Empty = implied (STFT: k*sr/N).
    // When present, size MUST equal bins and entries MUST be ordered.
    std::vector<float> bin_centers;
    // Representation schema version (NOT the dataset format version).
    // Bump when the interpretation of any field above changes.
    uint32_t version = 1;

    bool operator==(const RepresentationInfo& o) const {
        return kind == o.kind && bins == o.bins && fmin_hz == o.fmin_hz &&
               fmax_hz == o.fmax_hz && bands == o.bands && q == o.q &&
               norm == o.norm && phase == o.phase &&
               reassignment_supported == o.reassignment_supported &&
               bin_centers == o.bin_centers && version == o.version;
    }
    bool operator!=(const RepresentationInfo& o) const { return !(*this == o); }

    bool is_stft() const { return kind == RepresentationKind::STFT; }

    // STFT default: everything implied by the FFT grid.
    static RepresentationInfo stft_default() { return RepresentationInfo{}; }
};

inline const char* representation_kind_name(RepresentationKind k) {
    switch (k) {
        case RepresentationKind::STFT: return "stft";
        case RepresentationKind::Mel:  return "mel";
        case RepresentationKind::Bark: return "bark";
        case RepresentationKind::Erb:  return "erb";
        case RepresentationKind::Cqt:  return "cqt";
    }
    return "unknown";
}

inline bool try_parse_representation_kind(const std::string& s,
                                          RepresentationKind& out) {
    if (s == "stft") { out = RepresentationKind::STFT; return true; }
    if (s == "mel")  { out = RepresentationKind::Mel;  return true; }
    if (s == "bark") { out = RepresentationKind::Bark; return true; }
    if (s == "erb")  { out = RepresentationKind::Erb;  return true; }
    if (s == "cqt")  { out = RepresentationKind::Cqt;  return true; }
    return false;  // fail-closed: no silent STFT default
}

inline const char* representation_norm_name(RepresentationNorm n) {
    switch (n) {
        case RepresentationNorm::None:   return "none";
        case RepresentationNorm::Slaney: return "slaney";
        case RepresentationNorm::Area:   return "area";
    }
    return "unknown";
}

inline bool try_parse_representation_norm(const std::string& s,
                                          RepresentationNorm& out) {
    if (s == "none")   { out = RepresentationNorm::None;   return true; }
    if (s == "slaney") { out = RepresentationNorm::Slaney; return true; }
    if (s == "area")   { out = RepresentationNorm::Area;   return true; }
    return false;
}

// Fail-closed metadata validation. fft_size/sample_rate contextualize
// the STFT case (<= 0 = unknown: skip grid-exact checks, still check
// ranges). Returns true when valid; otherwise fills `errors`.
inline bool validate_representation(const RepresentationInfo& r, int fft_size,
                                    std::vector<std::string>& errors) {
    switch (r.kind) {
        case RepresentationKind::STFT:
        case RepresentationKind::Mel:
        case RepresentationKind::Bark:
        case RepresentationKind::Erb:
        case RepresentationKind::Cqt:
            break;
        default:
            errors.push_back("representation.kind is not a known kind");
            return false;
    }
    if (r.version == 0) {
        errors.push_back("representation.version must be > 0");
    }
    if (r.bins < 0) {
        errors.push_back("representation.bins must be >= 0");
    }
    if (r.fmin_hz < 0.0f || r.fmax_hz < 0.0f) {
        errors.push_back("representation frequency bounds must be >= 0");
    }
    if (r.fmax_hz > 0.0f && r.fmin_hz >= r.fmax_hz) {
        errors.push_back("representation.fmin_hz must be below fmax_hz");
    }
    if (r.is_stft()) {
        // STFT bins are implied by the FFT grid when unset; an explicit
        // count must match it exactly.
        if (fft_size > 0 && r.bins != 0 && r.bins != fft_size / 2 + 1) {
            errors.push_back("stft representation.bins must be fft_size/2+1");
        }
    } else {
        // Non-STFT bins are never derived from N/2+1: explicit count required.
        if (r.bins <= 0) {
            errors.push_back("non-STFT representation requires explicit bins");
        }
        if (!r.bin_centers.empty()) {
            if (static_cast<int>(r.bin_centers.size()) != r.bins) {
                errors.push_back("bin_centers size must equal bins");
            }
            for (size_t i = 0; i < r.bin_centers.size(); ++i) {
                if (!(r.bin_centers[i] >= 0.0f)) {
                    errors.push_back("bin_centers must be >= 0");
                    break;
                }
                if (i > 0 && r.bin_centers[i] < r.bin_centers[i - 1]) {
                    errors.push_back("bin_centers must be ordered");
                    break;
                }
            }
        }
        if (r.kind == RepresentationKind::Cqt) {
            if (r.fmin_hz <= 0.0f) {
                errors.push_back("cqt requires fmin_hz > 0");
            }
            if (r.fmax_hz <= r.fmin_hz) {
                errors.push_back("cqt requires fmax_hz > fmin_hz");
            }
            if (r.bands <= 0) {
                errors.push_back("cqt requires bands (bins per octave) > 0");
            }
        } else {
            if (r.bands < 0) {
                errors.push_back("representation.bands must be >= 0");
            }
        }
    }
    return errors.empty();
}

} // namespace Spectral
