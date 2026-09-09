// tests/spectral/test_representation_contract.cpp
// Phase 8 — representation-safety contracts. STFT keeps FFT-grid semantics
// and validates; synthetic non-STFT fixtures (NOT real Mel/CQT algorithms)
// prove generic code never assumes N/2+1, uniform spacing, phase, or
// reassignment; STFT-only operations refuse anything else explicitly.
#include "representation.h"
#include "spectral_dataset.h"
#include "spectrogram_renderer.h"
#include "spectrum_renderer.h"
#include "video_renderer.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace Spectral;

static int g_run = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++g_run;                                                             \
        if (cond) {                                                          \
            ++g_pass;                                                        \
        } else {                                                             \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);            \
        }                                                                    \
    } while (0)

static constexpr int kSr = 22050;
static constexpr int kFft = 1024;
static constexpr int kHop = 256;
static constexpr int kBins = kFft / 2 + 1;

// Full STFT fixture: grid axis, full frames, phase available.
static SpectralDataset make_stft(bool reassigned) {
    SpectralDataset d;
    d.mutable_frequency_axis() = FrequencyAxis(kFft, kSr);
    auto& am = d.mutable_analysis_metadata();
    am.fft_size = kFft;
    am.hop_size = kHop;
    am.sample_rate = kSr;
    am.num_frequency_bins = kBins;
    am.nyquist_frequency = kSr / 2.0f;
    am.analysis_method = "stft";
    auto& rep = d.mutable_representation();
    rep = RepresentationInfo::stft_default();
    rep.bins = kBins;
    rep.reassignment_supported = reassigned;
    d.mutable_time_axis() = TimeAxis(4, kHop, kSr);
    for (int f = 0; f < 4; ++f) {
        SpectralFrame fr;
        fr.frame_index = f;
        fr.n_fft = kFft;
        fr.timestamp = static_cast<double>(f * kHop) / kSr;
        fr.magnitudes.assign(kBins, 0.1f * (f + 1));
        fr.phases.assign(kBins, 0.01f * f);
        fr.power.assign(kBins, 0.0f);
        for (int k = 0; k < kBins; ++k)
            fr.power[k] = fr.magnitudes[k] * fr.magnitudes[k];
        if (reassigned) {
            fr.reassigned_times.assign(kBins, 0.001f * f);
            fr.reassigned_freqs.assign(kBins, 440.0f);
        }
        d.add_frame(fr);
    }
    return d;
}

// Synthetic Mel-like fixture: 64 explicit nonuniform centers, no phase,
// no reassignment. NOT a real filterbank — only a contract probe.
static SpectralDataset make_mel_like() {
    SpectralDataset d;
    const int bands = 64;
    std::vector<float> centers;
    for (int b = 0; b < bands; ++b) {
        const float t = static_cast<float>(b) / (bands - 1);
        centers.push_back(50.0f * std::pow(8000.0f / 50.0f, t));
    }
    d.mutable_frequency_axis() = FrequencyAxis::from_centers(centers, kSr);
    auto& am = d.mutable_analysis_metadata();
    am.fft_size = 2048;  // implementation parameter only, not bin source
    am.hop_size = 512;
    am.sample_rate = kSr;
    am.num_frequency_bins = bands;
    am.nyquist_frequency = 8000.0f;
    am.analysis_method = "synthetic-mel-probe";
    auto& rep = d.mutable_representation();
    rep.kind = RepresentationKind::Mel;
    rep.bins = bands;
    rep.fmin_hz = centers.front();
    rep.fmax_hz = centers.back();
    rep.bands = bands;
    rep.norm = RepresentationNorm::Slaney;
    rep.phase = RepresentationPhase::NotApplicable;
    rep.bin_centers = centers;
    d.mutable_time_axis() = TimeAxis(3, 512, kSr);
    for (int f = 0; f < 3; ++f) {
        SpectralFrame fr;
        fr.frame_index = f;
        fr.n_fft = 0;  // no FFT size for filterbank data
        fr.timestamp = static_cast<double>(f * 512) / kSr;
        fr.magnitudes.assign(bands, 0.2f * (f + 1));
        fr.power.assign(bands, 0.0f);
        for (int k = 0; k < bands; ++k) fr.power[k] = fr.magnitudes[k] * fr.magnitudes[k];
        d.add_frame(fr);
    }
    return d;
}

// Synthetic CQT-like fixture: log centers, fmin>0, bins-per-octave.
static SpectralDataset make_cqt_like() {
    const int bpo = 12, bands = bpo * 7;
    std::vector<float> centers;
    for (int b = 0; b < bands; ++b)
        centers.push_back(55.0f * std::pow(2.0f, static_cast<float>(b) / bpo));
    SpectralDataset d;
    d.mutable_frequency_axis() = FrequencyAxis::from_centers(centers, kSr);
    auto& am = d.mutable_analysis_metadata();
    am.fft_size = 2048;
    am.hop_size = 512;
    am.sample_rate = kSr;
    am.num_frequency_bins = bands;
    am.nyquist_frequency = 8000.0f;
    am.analysis_method = "synthetic-cqt-probe";
    auto& rep = d.mutable_representation();
    rep.kind = RepresentationKind::Cqt;
    rep.bins = bands;
    rep.fmin_hz = centers.front();
    rep.fmax_hz = centers.back();
    rep.bands = bpo;
    rep.q = 34.0f;
    rep.norm = RepresentationNorm::None;
    rep.phase = RepresentationPhase::NotApplicable;
    rep.bin_centers = centers;
    d.mutable_time_axis() = TimeAxis(0, 512, kSr);
    for (int f = 0; f < 3; ++f) {
        SpectralFrame fr;
        fr.frame_index = f;
        fr.timestamp = static_cast<double>(f * 512) / kSr;
        fr.magnitudes.assign(bands, 0.15f);
        fr.power.assign(bands, 0.0225f);
        d.add_frame(fr);
    }
    return d;
}

static void test_stft_valid() {
    std::printf("[stft_valid]\n");
    CHECK(make_stft(false).validate().valid, "plain STFT validates");
    CHECK(make_stft(true).validate().valid, "reassigned STFT validates");
}

static void test_stft_invalid() {
    std::printf("[stft_invalid]\n");
    {
        auto d = make_stft(false);
        d.mutable_frequency_axis().num_bins = kBins - 1;  // lie about width
        CHECK(!d.validate().valid, "wrong bin count rejected");
    }
    {
        auto d = make_stft(false);
        d.mutable_frequency_axis().bin_frequencies.pop_back();  // axis short
        CHECK(!d.validate().valid, "short axis rejected");
    }
    {
        auto d = make_stft(false);
        d.mutable_representation().phase = RepresentationPhase::NotApplicable;
        CHECK(!d.validate().valid, "populated phases with phase N/A rejected");
    }
    {
        auto d = make_stft(false);  // support off, data on
        auto& f = const_cast<SpectralFrame&>(d.frame(0));
        f.reassigned_times.assign(kBins, 0.0f);
        f.reassigned_freqs.assign(kBins, 0.0f);
        CHECK(!d.validate().valid, "reassigned data without support rejected");
    }
    {
        auto d = make_stft(true);
        auto& f = const_cast<SpectralFrame&>(d.frame(1));
        f.reassigned_times.pop_back();  // corrupt length
        CHECK(!d.validate().valid, "short reassigned array rejected");
    }
    {
        auto d = make_stft(false);
        d.mutable_representation().fmin_hz = 100.0f;  // axis starts at 0
        d.mutable_representation().fmax_hz = 11000.0f;
        CHECK(!d.validate().valid, "contradictory range rejected");
    }
    {
        auto d = make_stft(false);
        const_cast<SpectralFrame&>(d.frame(2)).n_fft = 512;  // grid mismatch
        CHECK(!d.validate().valid, "frame n_fft mismatch rejected");
    }
    {
        auto d = make_stft(false);
        d.mutable_normalization_info().phase_unwrapped = true;  // phase flagged w/o capability
        d.mutable_representation().phase = RepresentationPhase::NotApplicable;
        auto& f = const_cast<SpectralFrame&>(d.frame(0));
        f.phases.clear();
        CHECK(!d.validate().valid, "phase norm without capability rejected");
    }
}

static void test_nonstft_valid() {
    std::printf("[nonstft_valid]\n");
    CHECK(make_mel_like().validate().valid, "mel-like validates");
    CHECK(make_cqt_like().validate().valid, "cqt-like validates");
}

static void test_nonstft_invalid() {
    std::printf("[nonstft_invalid]\n");
    {
        auto d = make_mel_like();
        d.mutable_representation().bins = 0;  // explicit count required
        CHECK(!d.validate().valid, "mel bins=0 rejected");
    }
    {
        auto d = make_mel_like();
        auto& c = d.mutable_frequency_axis().bin_frequencies;
        std::swap(c[10], c[40]);  // unordered centers
        CHECK(!d.validate().valid, "unordered centers rejected");
    }
    {
        auto d = make_cqt_like();
        d.mutable_representation().bands = 0;
        CHECK(!d.validate().valid, "cqt without bands rejected");
    }
    {
        auto d = make_cqt_like();
        d.mutable_representation().fmin_hz = 0.0f;
        CHECK(!d.validate().valid, "cqt fmin=0 rejected");
    }
    {
        auto d = make_mel_like();
        auto& f = const_cast<SpectralFrame&>(d.frame(0));
        f.phases.assign(64, 0.0f);  // fabricated phase
        CHECK(!d.validate().valid, "mel with phases rejected");
    }
}

static void test_transforms() {
    std::printf("[transforms]\n");
    {
        auto d = make_stft(false);
        auto s = d.filter_band(1000.0f, 4000.0f);
        CHECK(s.validate().valid, "stft slice validates");
        CHECK(s.representation().kind == RepresentationKind::STFT, "slice keeps kind");
        CHECK(s.num_frequency_bins() < d.num_frequency_bins(), "slice narrows");
        CHECK(s.representation().fmin_hz == s.frequency_axis().bin_frequencies.front(),
              "slice range follows axis");
        CHECK(s.frame_count() == d.frame_count(), "slice keeps frames");
    }
    {
        auto d = make_mel_like();
        auto s = d.filter_band(200.0f, 2000.0f);
        CHECK(s.validate().valid, "mel slice validates");
        CHECK(s.representation().bins == s.num_frequency_bins(), "mel bins updated");
        CHECK(s.representation().bin_centers == s.frequency_axis().bin_frequencies,
              "mel centers tracked");
        CHECK(s.frequency_axis().nyquist == s.frequency_axis().bin_frequencies.back(),
              "mel nyquist follows last center");
        for (int f = 0; f < s.frame_count(); ++f)
            CHECK(s.frame(f).phases.empty(), "mel slice stays phaseless");
    }
    {
        auto d = make_stft(false);
        auto s = d.downsample_frequency(4);
        CHECK(s.validate().valid, "stft decimation validates");
        CHECK(s.representation().fmin_hz == s.frequency_axis().bin_frequencies.front(),
              "decimated range explicit");
    }
    {
        auto d = make_mel_like();
        auto s = d.downsample_frequency(2);
        CHECK(s.validate().valid, "mel decimation validates");
        CHECK(s.num_frequency_bins() == 32, "mel decimation halves");
    }
    {
        auto d = make_mel_like();
        auto t = d.filter_time(0.0, 100.0);
        CHECK(t.validate().valid, "mel time crop validates");
        CHECK(t.representation().kind == RepresentationKind::Mel, "crop keeps kind");
        CHECK(t.export_csv("phase8_csv_tmp.csv"), "mel csv exports");
        // Phase column must be empty (no fabricated values).
        bool phase_empty = false;
        {
            std::ifstream f("phase8_csv_tmp.csv");
            std::string header, first;
            if (std::getline(f, header) && std::getline(f, first)) {
                // frame,timestamp,freq,mag,phase,power -> ",," before power
                const size_t c4 = first.rfind(',');
                const size_t c3 = first.rfind(',', c4 - 1);
                phase_empty = (c4 != std::string::npos && c3 != std::string::npos &&
                               c4 == c3 + 1);
            }
        }
        CHECK(phase_empty, "csv phase column empty for phaseless data");
        std::remove("phase8_csv_tmp.csv");
    }
    {
        auto d = make_mel_like();
        CHECK(d.mean_magnitude_spectrum().size() == 64, "stats cover bins");
        CHECK(d.max_magnitude_spectrum().size() == 64, "max covers bins");
    }
}

static void test_refusals() {
    std::printf("[refusals]\n");
    auto mel = make_mel_like();
    {
        SpectrogramRenderer r;
        RGBAImage img;
        CHECK(r.render(mel, img) == RenderError::UnsupportedRepresentation,
              "spectrogram refuses mel");
        CHECK(!img.valid(), "no partial image on refusal");
    }
    {
        SpectrumRenderer r;
        RGBAImage img;
        CHECK(r.render(mel, img) == SpectrumError::UnsupportedRepresentation,
              "spectrum refuses mel");
    }
    {
        VideoRenderer vr;
        CHECK(vr.render(mel, "phase8_refuse_tmp.mp4") ==
                  VideoRenderError::UnsupportedRepresentation,
              "video refuses mel");
        RGBAImage frame;
        CHECK(vr.render_frame(mel, 0.0, frame) ==
                  VideoRenderError::UnsupportedRepresentation,
              "video frame refuses mel");
    }
    {
        // Binary is STFT-only: non-STFT fails closed, never mislabeled.
        std::vector<uint8_t> buf;
        CHECK(!mel.serialize_binary(buf), "mel binary refused");
        CHECK(mel.dataset_identity().empty(), "mel has no identity");
    }
}

static void test_serialization_roundtrip() {
    std::printf("[serialization_roundtrip]\n");
    {
        auto d = make_stft(true);
        std::vector<uint8_t> buf;
        CHECK(d.serialize_binary(buf), "stft binary writes");
        SpectralDataset loaded;
        CHECK(loaded.deserialize_binary(buf.data(), buf.size()), "stft binary loads");
        CHECK(loaded.validate().valid, "loaded stft validates");
        CHECK(loaded == d, "stft binary round-trips");
        CHECK(loaded.dataset_identity() == d.dataset_identity(), "identity stable");
    }
    {
        // Binary into a reused non-STFT object resets to STFT (v3 is STFT-only).
        auto d = make_stft(false);
        std::vector<uint8_t> buf;
        CHECK(d.serialize_binary(buf), "stft binary writes");
        SpectralDataset reused = make_mel_like();
        CHECK(reused.deserialize_binary(buf.data(), buf.size()), "binary loads");
        CHECK(reused.representation().is_stft(), "representation reset to STFT");
        CHECK(reused.validate().valid, "reset object validates");
    }
    {
        auto d = make_mel_like();
        const std::string js = d.serialize_json();
        SpectralDataset loaded;
        CHECK(loaded.deserialize_json(js), "mel json loads");
        CHECK(loaded.validate().valid, "loaded mel validates");
        CHECK(loaded.representation() == d.representation(), "mel rep round-trips");
        CHECK(loaded.analysis_metadata().analysis_method == "synthetic-mel-probe",
              "method round-trips");
        CHECK(loaded == d, "mel json round-trips");
    }
    {
        // JSON without a representation section means STFT default.
        auto d = make_stft(false);
        std::string js = d.serialize_json();
        // strip the representation section textually (present at tail)
        const size_t pos = js.find("\"representation\"");
        CHECK(pos != std::string::npos, "section present to strip");
        SpectralDataset reused = make_mel_like();
        // parse a hand-minimal STFT json: reuse full json minus rep section
        std::string cut = js;
        const size_t brace = cut.find('{', pos);
        int depth = 0;
        size_t q = brace;
        for (; q < cut.size(); ++q) {
            if (cut[q] == '{') ++depth;
            else if (cut[q] == '}') {
                --depth;
                if (depth == 0) {
                    ++q;
                    break;
                }
            }
        }
        cut.erase(pos - 1, q - pos + 1);  // drop ,"representation":{...}
        CHECK(reused.deserialize_json(cut), "json without rep loads");
        CHECK(reused.representation().is_stft(), "absent section means STFT");
    }
    {
        // method survives even for STFT (was dropped before this phase).
        auto d = make_stft(false);
        d.mutable_analysis_metadata().analysis_method = "multiband_stft";
        SpectralDataset loaded;
        CHECK(loaded.deserialize_json(d.serialize_json()), "stft json loads");
        CHECK(loaded.analysis_metadata().analysis_method == "multiband_stft",
              "stft method round-trips");
    }
    {
        auto a = make_stft(false);
        auto b = make_stft(false);
        CHECK(a.dataset_identity() == b.dataset_identity(), "same content same identity");
        // Sliced (explicit-range) STFT differs in axis bytes -> identity differs.
        auto s = a.filter_band(1000.0f, 4000.0f);
        CHECK(s.dataset_identity() != a.dataset_identity(),
              "narrowed range changes identity");
    }
}

int main() {
    test_stft_valid();
    test_stft_invalid();
    test_nonstft_valid();
    test_nonstft_invalid();
    test_transforms();
    test_refusals();
    test_serialization_roundtrip();
    std::printf("\n=== representation_contract: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
