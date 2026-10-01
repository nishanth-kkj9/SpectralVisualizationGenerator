// tests/spectral/test_representation_contract.cpp
// Phase 8 — representation-safety contracts. STFT keeps FFT-grid semantics
// and validates; synthetic non-STFT fixtures (NOT real Mel/CQT algorithms)
// prove generic code never assumes N/2+1, uniform spacing, phase, or
// reassignment; STFT-only operations refuse anything else explicitly.
#include "mel_filterbank.h"
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

// Real Mel-shaped fixture: a genuine triangular Mel filterbank built by the
// same production builder the pipeline uses, so the declared coverage below
// is the actual first-left / last-right filter edge rather than an arbitrary
// number. Every center lies strictly inside that coverage, which is what
// makes a "coverage := retained center span" regression detectable.
// (make_mel_like() above is an explicit contract probe, not a filterbank;
// these transform tests need real filter semantics.)
static SpectralDataset make_real_mel() {
    MelFilterbankConfig mc;
    mc.sample_rate = kSr;
    mc.fft_size = kFft;
    mc.fmin_hz = 80.0f;
    mc.fmax_hz = 7000.0f;
    mc.bands = 24;
    mc.norm = RepresentationNorm::Slaney;
    MelFilterbank bank;
    std::string err;
    if (!bank.build(mc, err)) return SpectralDataset{};

    SpectralDataset d;
    d.mutable_frequency_axis() = FrequencyAxis::from_centers(bank.centers_hz(), kSr);
    auto& am = d.mutable_analysis_metadata();
    am.fft_size = kFft;
    am.hop_size = kHop;
    am.sample_rate = kSr;
    am.num_frequency_bins = bank.bands();
    am.nyquist_frequency = kSr / 2.0f;
    am.analysis_method = "mel";
    auto& rep = d.mutable_representation();
    rep.kind = RepresentationKind::Mel;
    rep.bins = bank.bands();
    rep.fmin_hz = bank.fmin_hz();  // first filter's left coverage edge
    rep.fmax_hz = bank.fmax_hz();  // last filter's right coverage edge
    rep.bands = bank.bands();
    rep.norm = RepresentationNorm::Slaney;
    rep.phase = RepresentationPhase::NotApplicable;
    rep.reassignment_supported = false;
    rep.bin_centers = bank.centers_hz();
    d.mutable_time_axis() = TimeAxis(0, kHop, kSr);
    for (int f = 0; f < 2; ++f) {
        SpectralFrame fr;
        fr.frame_index = f;
        fr.n_fft = 0;  // no FFT size for filterbank data
        fr.timestamp = static_cast<double>(f * kHop) / kSr;
        fr.magnitudes.assign(static_cast<size_t>(bank.bands()), 0.2f * (f + 1));
        fr.power.assign(static_cast<size_t>(bank.bands()), 0.0f);
        for (int b = 0; b < bank.bands(); ++b)
            fr.power[static_cast<size_t>(b)] =
                fr.magnitudes[static_cast<size_t>(b)] *
                fr.magnitudes[static_cast<size_t>(b)];
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
        // The real Mel contract pins bands explicitly and agrees with bins.
        auto d = make_mel_like();
        d.mutable_representation().bands = 0;
        CHECK(!d.validate().valid, "mel bands=0 rejected");
    }
    {
        auto d = make_mel_like();
        d.mutable_representation().bins = 32;  // bands stay 64
        CHECK(!d.validate().valid, "mel bins!=bands rejected");
    }
    {
        auto d = make_mel_like();
        d.mutable_representation().reassignment_supported = true;
        CHECK(!d.validate().valid, "mel reassignment rejected");
    }
    {
        auto d = make_mel_like();
        d.mutable_representation().norm = static_cast<RepresentationNorm>(99);
        CHECK(!d.validate().valid, "mel invalid norm rejected");
    }
    {
        // Duplicate (non-strictly-increasing) centers are invalid Mel
        // geometry, even though the generic axis order check would pass.
        auto d = make_mel_like();
        auto& c = d.mutable_representation().bin_centers;
        c[10] = c[11];
        CHECK(!d.validate().valid, "mel duplicate centers rejected");
    }
    {
        auto d = make_mel_like();
        auto& c = d.mutable_representation().bin_centers;
        c[20] = -5.0f;
        CHECK(!d.validate().valid, "mel negative centers rejected");
    }
    {
        auto d = make_mel_like();
        auto& c = d.mutable_representation().bin_centers;
        c.pop_back();  // size no longer equals bins
        CHECK(!d.validate().valid, "mel short centers rejected");
    }
    {
        // Valid Mel metadata (the fixture itself) still passes.
        CHECK(make_mel_like().validate().valid, "valid mel still passes");
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
        // Phase 9: video became representation-aware — a Mel dataset renders
        // through the Mel renderer (explicit centers to pixels) instead of
        // being refused. The STFT-only guard itself is unchanged and still
        // rejects every other kind (Bark probed below).
        VideoRenderer vr;
        RGBAImage frame;
        CHECK(vr.render_frame(mel, 0.0, frame) == VideoRenderError::Ok,
              "video frame renders mel via the mel renderer");
        CHECK(frame.valid(), "mel video frame produced an image");
        SpectralDataset bark = mel;
        bark.mutable_representation().kind = RepresentationKind::Bark;
        RGBAImage refused;
        CHECK(vr.render_frame(bark, 0.0, refused) ==
                  VideoRenderError::UnsupportedRepresentation,
              "video frame refuses other non-stft kinds");
        CHECK(!refused.valid(), "no partial image for a refused video frame");
    }
    {
        // Encoding Mel frames needs ffmpeg; skip like the codebase's other
        // video tests do.
        if (VideoEncoder::ffmpeg_available()) {
            VideoRenderer vr;
            CHECK(vr.render(mel, "phase8_mel_tmp.mp4") == VideoRenderError::Ok,
                  "video encodes mel");
            std::remove("phase8_mel_tmp.mp4");
        } else {
            std::printf("  (ffmpeg missing: mel video encode skipped)\n");
        }
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

// Mel filterbank coverage must survive band selection. filter_band() and
// downsample_frequency() drop bands of the SAME filterbank, so every retained
// band keeps its original support and the declared coverage edges are
// unchanged. Rewriting fmin_hz/fmax_hz to the retained center span (the
// pre-fix behavior) relabels a filter center as a coverage edge — claiming
// coverage the retained filters do not have. These assertions inspect the
// metadata itself, not merely validate(), and each one fails on that behavior.
static void test_mel_transform_coverage() {
    std::printf("[mel_transform_coverage]\n");
    const SpectralDataset mel = make_real_mel();
    CHECK(mel.validate().valid, "real mel fixture validates");
    CHECK(mel.num_frequency_bins() == 24, "fixture band count");
    const float cov_min = mel.representation().fmin_hz;
    const float cov_max = mel.representation().fmax_hz;
    CHECK(cov_min == 80.0f && cov_max == 7000.0f,
          "fixture coverage = first-left .. last-right filter edge");
    CHECK(cov_min < mel.representation().bin_centers.front(),
          "coverage starts below the first center");
    CHECK(cov_max > mel.representation().bin_centers.back(),
          "coverage ends above the last center");

    // --- filter_band(): contiguous band selection --------------------------
    {
        const std::vector<float>& c = mel.representation().bin_centers;
        const SpectralDataset s = mel.filter_band(c[5], c[11]);
        const RepresentationInfo& rep = s.representation();
        CHECK(s.num_frequency_bins() == 7, "slice keeps bands 5..11");
        CHECK(s.validate().valid, "mel slice validates");
        CHECK(rep.kind == RepresentationKind::Mel, "slice keeps kind");
        CHECK(rep.bins == 7, "slice bins = retained bands");
        CHECK(rep.bands == 7, "slice bands stay synchronized with bins");
        CHECK(rep.bin_centers == std::vector<float>(c.begin() + 5, c.begin() + 12),
              "slice centers are exactly the retained centers");
        CHECK(rep.fmin_hz == cov_min, "slice preserves coverage floor");
        CHECK(rep.fmax_hz == cov_max, "slice preserves coverage ceiling");
        CHECK(rep.fmin_hz != rep.bin_centers.front() &&
                  rep.fmax_hz != rep.bin_centers.back(),
              "slice does not relabel retained centers as coverage edges");
        CHECK(rep.fmin_hz < rep.bin_centers.front() &&
                  rep.fmax_hz > rep.bin_centers.back(),
              "retained centers stay inside the preserved coverage");
        CHECK(rep.phase == RepresentationPhase::NotApplicable,
              "slice stays phase N/A");
        CHECK(!rep.reassignment_supported, "slice keeps reassignment off");
        for (int f = 0; f < s.frame_count(); ++f)
            CHECK(s.frame(f).phases.empty(), "slice frames stay phaseless");
        // JSON must carry the corrected metadata, not "repair" it.
        SpectralDataset loaded;
        CHECK(loaded.deserialize_json(s.serialize_json()), "sliced mel json loads");
        CHECK(loaded.representation() == rep,
              "sliced mel representation round-trips");
        CHECK(loaded.validate().valid, "loaded sliced mel validates");
        CHECK(loaded.representation().fmin_hz == cov_min &&
                  loaded.representation().fmax_hz == cov_max,
              "round-trip keeps coverage, not the center span");
    }

    // --- downsample_frequency(): strided band selection --------------------
    {
        const std::vector<float>& c = mel.representation().bin_centers;
        const SpectralDataset s = mel.downsample_frequency(3);
        const RepresentationInfo& rep = s.representation();
        std::vector<float> keep;
        for (size_t k = 0; k < c.size(); k += 3) keep.push_back(c[k]);
        CHECK(keep.size() == 8, "stride-3 keeps 8 of 24 bands");
        CHECK(s.validate().valid, "mel decimation validates");
        CHECK(rep.bins == 8, "decimated bins");
        CHECK(rep.bands == 8, "decimated bands stay synchronized with bins");
        CHECK(rep.bin_centers == keep, "decimated centers are the strided centers");
        CHECK(rep.fmin_hz == cov_min, "decimation preserves coverage floor");
        CHECK(rep.fmax_hz == cov_max, "decimation preserves coverage ceiling");
        CHECK(rep.fmin_hz != rep.bin_centers.front() &&
                  rep.fmax_hz != rep.bin_centers.back(),
              "decimation does not relabel retained centers as coverage edges");
        CHECK(rep.phase == RepresentationPhase::NotApplicable,
              "decimation stays phase N/A");
    }

    // --- STFT contrast: there the range IS the kept center span -----------
    {
        const SpectralDataset d = make_stft(false);
        const SpectralDataset s = d.filter_band(1000.0f, 4000.0f);
        CHECK(s.representation().fmin_hz ==
                      s.frequency_axis().bin_frequencies.front() &&
                  s.representation().fmax_hz ==
                      s.frequency_axis().bin_frequencies.back(),
              "stft range still follows the kept centers");
        CHECK(s.representation().bins == 0, "stft count still implied after slice");
        const SpectralDataset t = d.downsample_frequency(4);
        CHECK(t.representation().fmin_hz ==
                      t.frequency_axis().bin_frequencies.front() &&
                  t.representation().fmax_hz ==
                      t.frequency_axis().bin_frequencies.back(),
              "stft decimated range still follows the kept centers");
    }
}

int main() {
    test_stft_valid();
    test_stft_invalid();
    test_nonstft_valid();
    test_nonstft_invalid();
    test_transforms();
    test_mel_transform_coverage();
    test_refusals();
    test_serialization_roundtrip();
    std::printf("\n=== representation_contract: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
