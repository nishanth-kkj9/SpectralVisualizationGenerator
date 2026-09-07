// tests/spectral/test_representation.cpp
// S6.0 architecture/contract tests: representation kinds are distinct and
// stable, STFT keeps FFT-bin semantics, non-STFT kinds never derive bins
// from N/2+1, invalid input fails closed, and the analysis fingerprint
// tracks representation (not rendering, not paths).
#include "project_config.h"
#include "spectral_dataset.h"

#include <cstdio>
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

// A. Enumeration: distinct, stable, round-trippable, fail-closed parse.
static void test_enumeration() {
    std::printf("[enumeration]\n");
    CHECK(static_cast<int>(RepresentationKind::STFT) == 0, "stft stable");
    CHECK(static_cast<int>(RepresentationKind::Mel) == 1, "mel stable");
    CHECK(static_cast<int>(RepresentationKind::Bark) == 2, "bark stable");
    CHECK(static_cast<int>(RepresentationKind::Erb) == 3, "erb stable");
    CHECK(static_cast<int>(RepresentationKind::Cqt) == 4, "cqt stable");
    const char* names[] = {"stft", "mel", "bark", "erb", "cqt"};
    RepresentationKind kinds[] = {RepresentationKind::STFT, RepresentationKind::Mel,
                                  RepresentationKind::Bark, RepresentationKind::Erb,
                                  RepresentationKind::Cqt};
    for (int i = 0; i < 5; ++i) {
        CHECK(std::string(representation_kind_name(kinds[i])) == names[i], "name stable");
        RepresentationKind back = RepresentationKind::STFT;
        CHECK(try_parse_representation_kind(names[i], back) && back == kinds[i], "parse round trip");
    }
    RepresentationKind bad = RepresentationKind::STFT;
    CHECK(!try_parse_representation_kind("", bad), "empty rejected");
    CHECK(!try_parse_representation_kind("STFT", bad), "case rejected");
    CHECK(!try_parse_representation_kind("fft", bad), "fft rejected");
    CHECK(!try_parse_representation_kind("mel-scale", bad), "near-miss rejected");
    RepresentationNorm n = RepresentationNorm::None;
    CHECK(!try_parse_representation_norm("loud", n), "norm rejected");
}

// B. STFT metadata keeps FFT-bin semantics.
static void test_stft_metadata() {
    std::printf("[stft_metadata]\n");
    const RepresentationInfo r = RepresentationInfo::stft_default();
    CHECK(r.is_stft(), "default is STFT");
    CHECK(r.bins == 0, "bins implied by FFT grid");
    CHECK(r.phase == RepresentationPhase::Available, "STFT phase available");
    std::vector<std::string> errs;
    CHECK(validate_representation(r, 1024, errs), "STFT default validates");
    SpectralDataset d;
    d.mutable_analysis_metadata().fft_size = 1024;
    d.mutable_analysis_metadata().hop_size = 512;
    d.mutable_analysis_metadata().sample_rate = 44100;
    d.mutable_analysis_metadata().num_frequency_bins = 513;
    d.mutable_frequency_axis() = FrequencyAxis(1024, 44100);
    // representation_ defaults to STFT: must validate clean
    auto v = d.validate();
    bool rep_ok = true;
    for (const auto& e : v.errors)
        if (e.find("representation") != std::string::npos) rep_ok = false;
    CHECK(rep_ok, "STFT dataset representation validates");
}

// C. Non-STFT metadata never derives bins from N/2+1.
static void test_non_stft_metadata() {
    std::printf("[non_stft_metadata]\n");
    std::vector<std::string> errs;
    for (auto [kind, bands] :
         {std::pair<RepresentationKind, int>{RepresentationKind::Mel, 64},
          {RepresentationKind::Bark, 24}, {RepresentationKind::Erb, 32}}) {
        RepresentationInfo r;
        r.kind = kind;
        r.bins = bands;  // 64 bands with fft 1024 (N/2+1 would be 513)
        r.fmin_hz = 20.0f;
        r.fmax_hz = 8000.0f;
        r.bands = bands;
        r.phase = RepresentationPhase::NotApplicable;
        errs.clear();
        CHECK(validate_representation(r, 1024, errs), "explicit non-STFT bins accepted");
    }
    RepresentationInfo cqt;
    cqt.kind = RepresentationKind::Cqt;
    cqt.bins = 84;
    cqt.fmin_hz = 55.0f;
    cqt.fmax_hz = 7040.0f;
    cqt.bands = 12;
    cqt.q = 0.0f;  // derived later; 0 allowed
    cqt.phase = RepresentationPhase::NotApplicable;
    errs.clear();
    CHECK(validate_representation(cqt, 0, errs), "CQT metadata accepted");
    // missing explicit bins rejected even though an FFT size exists
    RepresentationInfo bare;
    bare.kind = RepresentationKind::Mel;
    errs.clear();
    CHECK(!validate_representation(bare, 1024, errs), "Mel without bins rejected");
}

// D. Invalid representation fails closed.
static void test_invalid_rejected() {
    std::printf("[invalid_rejected]\n");
    std::vector<std::string> errs;
    RepresentationInfo bad;
    bad.kind = static_cast<RepresentationKind>(99);
    CHECK(!validate_representation(bad, 1024, errs), "unknown kind rejected");
    RepresentationInfo v0;
    v0.version = 0;
    CHECK(!validate_representation(v0, 1024, errs), "version 0 rejected");
    RepresentationInfo neg;
    neg.kind = RepresentationKind::Mel;
    neg.bins = 64;
    neg.fmin_hz = 9000.0f;
    neg.fmax_hz = 100.0f;
    CHECK(!validate_representation(neg, 0, errs), "inverted range rejected");
    RepresentationInfo cq;
    cq.kind = RepresentationKind::Cqt;
    CHECK(!validate_representation(cq, 0, errs), "bare CQT rejected");
    RepresentationInfo stft_bad_bins;
    stft_bad_bins.bins = 100;  // explicit but != 513
    CHECK(!validate_representation(stft_bad_bins, 1024, errs), "wrong STFT bins rejected");
}

// Shared config builder: identical except for the parameter under test.
static ProjectConfig base_cfg() {
    ProjectConfig c;
    c.input.file_hash = "aa";
    c.input.file_size_bytes = 100;
    c.analysis.fft_size = 1024;
    c.analysis.hop_size = 512;
    c.analysis.sample_rate = 44100;
    return c;
}

// E/F/G. Fingerprint tracks representation + params, not rendering/paths.
static void test_fingerprint_tracks_representation() {
    std::printf("[fingerprint_tracks_representation]\n");
    const std::string a0 = base_cfg().analysis_fingerprint();
    auto mel = base_cfg();
    mel.analysis.representation.kind = RepresentationKind::Mel;
    mel.analysis.representation.bins = 64;
    mel.analysis.representation.fmin_hz = 20.0f;
    mel.analysis.representation.fmax_hz = 8000.0f;
    mel.analysis.representation.bands = 64;
    mel.analysis.representation.phase = RepresentationPhase::NotApplicable;
    CHECK(mel.analysis_fingerprint() != a0, "STFT->Mel moves fingerprint");
    auto mel128 = mel;
    mel128.analysis.representation.bins = 128;
    mel128.analysis.representation.bands = 128;
    CHECK(mel128.analysis_fingerprint() != mel.analysis_fingerprint(), "bands move it");
    auto cq = base_cfg();
    cq.analysis.representation.kind = RepresentationKind::Cqt;
    cq.analysis.representation.bins = 84;
    cq.analysis.representation.fmin_hz = 55.0f;
    cq.analysis.representation.fmax_hz = 7040.0f;
    cq.analysis.representation.bands = 12;
    const std::string c0 = cq.analysis_fingerprint();
    auto cq24 = cq;
    cq24.analysis.representation.q = 24.0f;
    CHECK(cq24.analysis_fingerprint() != c0, "Q moves it");
    auto wide = base_cfg();
    wide.renderer.width = 2048;
    CHECK(wide.analysis_fingerprint() == a0, "image width does not move it");
    auto heat = base_cfg();
    heat.renderer.color_map = ProjectColorMap::Heat;
    CHECK(heat.analysis_fingerprint() == a0, "color map does not move it");
    // representation round-trips through JSON
    ProjectConfig out;
    std::string err;
    CHECK(ProjectConfigSerializer::from_json(ProjectConfigSerializer::to_json(mel), out, err),
          "mel config round trips");
    CHECK(out == mel, "mel config equality");
    CHECK(out.analysis_fingerprint() == mel.analysis_fingerprint(), "fp stable across RT");
}

// H. Path independence intact (same hash, different path).
static void test_path_independence() {
    std::printf("[path_independence]\n");
    auto a = base_cfg();
    a.input.file_path = "/data/a.wav";
    auto b = base_cfg();
    b.input.file_path = "/other/b.wav";
    CHECK(a.analysis_fingerprint() == b.analysis_fingerprint(), "path never moves fp");
}

static SpectralDataset make_mel64() {
    // Valid semantic shape: representation bins = axis bins = metadata
    // bins = frame width = 64; phases empty (N/A); explicit centers.
    SpectralDataset d;
    d.mutable_analysis_metadata().fft_size = 1024;
    d.mutable_analysis_metadata().hop_size = 512;
    d.mutable_analysis_metadata().sample_rate = 44100;
    d.mutable_analysis_metadata().num_frequency_bins = 64;
    std::vector<float> centers(64);
    for (int k = 0; k < 64; ++k) centers[static_cast<size_t>(k)] = 20.0f + k * 125.0f;
    d.mutable_frequency_axis() = FrequencyAxis::from_centers(centers, 44100);
    d.mutable_time_axis() = TimeAxis(2, 512, 44100);
    auto& rep = d.mutable_representation();
    rep.kind = RepresentationKind::Mel;
    rep.bins = 64;
    rep.fmin_hz = 20.0f;
    rep.fmax_hz = 20.0f + 63 * 125.0f;
    rep.bands = 64;
    rep.phase = RepresentationPhase::NotApplicable;
    for (int i = 0; i < 2; ++i) {
        SpectralFrame f;
        f.frame_index = i;
        f.n_fft = 0;  // meaningless for Mel; must not be checked
        f.timestamp = i * 512.0 / 44100.0;
        f.magnitudes.assign(64, 0.1f);
        f.power.assign(64, 0.01f);
        // phases stays empty: N/A means N/A
        d.add_frame(f);
    }
    return d;
}

static void test_dataset_coherence() {
    std::printf("[dataset_coherence]\n");
    SpectralDataset d = make_mel64();
    CHECK(!d.validate().has_errors(), "mel-shaped dataset validates");
    SpectralDataset bad = d;
    SpectralFrame extra;
    extra.frame_index = 9;
    extra.magnitudes.assign(513, 0.1f);  // FFT-shaped frame in Mel data
    extra.power.assign(513, 0.01f);
    bad.add_frame(extra);
    CHECK(bad.validate().has_errors(), "FFT-sized frame in Mel rejected");
    SpectralDataset phased = d;
    SpectralFrame pf;
    phased.get_frame(0, pf);
    pf.phases.assign(64, 0.0f);  // fabricated phase under N/A
    // replace frame 0: rebuild (frames are value-stored)
    SpectralDataset ph2;
    ph2.mutable_analysis_metadata() = phased.analysis_metadata();
    ph2.mutable_frequency_axis() = phased.frequency_axis();
    ph2.mutable_time_axis() = phased.time_axis();
    ph2.mutable_channel_info() = phased.channel_info();
    ph2.mutable_normalization_info() = phased.normalization_info();
    ph2.mutable_source_metadata() = phased.source_metadata();
    ph2.mutable_representation() = phased.representation();
    ph2.add_frame(pf);
    SpectralFrame f1;
    phased.get_frame(1, f1);
    ph2.add_frame(f1);
    CHECK(ph2.validate().has_errors(), "populated phase under N/A rejected");
    // representation survives the JSON path (binary waits for v4).
    SpectralDataset rt;
    CHECK(rt.deserialize_json(d.serialize_json(false)), "json loads");
    CHECK(rt.representation() == d.representation(), "representation JSON RT");
    CHECK(!rt.validate().has_errors(), "reloaded validates");
}

static void test_v3_and_identity_gates() {
    std::printf("[v3_and_identity_gates]\n");
    // J: non-STFT datasets fail closed on v3 binary serialization.
    SpectralDataset mel = make_mel64();
    std::vector<uint8_t> buf;
    CHECK(!mel.serialize_binary(buf), "Mel binary rejected");
    CHECK(buf.empty(), "no partial bytes emitted");
    // K: non-STFT identity fails safe (empty), never a fake hash.
    CHECK(mel.dataset_identity().empty(), "Mel identity unavailable");
    // L/M: STFT behavior unchanged (covered in depth by the spectral
    // suite; pinned here so the gates above cannot regress it).
    SpectralDataset stft;
    stft.mutable_analysis_metadata().fft_size = 8;
    stft.mutable_analysis_metadata().hop_size = 4;
    stft.mutable_analysis_metadata().sample_rate = 8000;
    stft.mutable_analysis_metadata().num_frequency_bins = 5;
    stft.mutable_frequency_axis() = FrequencyAxis(8, 8000);
    stft.mutable_time_axis() = TimeAxis(1, 4, 8000);
    SpectralFrame f;
    f.frame_index = 0;
    f.n_fft = 8;
    f.magnitudes.assign(5, 0.2f);
    f.phases.assign(5, 0.0f);
    f.power.assign(5, 0.04f);
    stft.add_frame(f);
    CHECK(stft.serialize_binary(buf), "STFT binary still works");
    CHECK(stft.dataset_identity().size() == 64, "STFT identity intact");
    // I: reassignment arrays without support are rejected.
    SpectralDataset bad = make_mel64();  // support defaults to false
    SpectralFrame bf;
    bad.get_frame(0, bf);
    bf.reassigned_times.assign(64, 0.0f);
    bf.reassigned_freqs.assign(64, 0.0f);
    SpectralDataset bad2;
    bad2.mutable_analysis_metadata() = bad.analysis_metadata();
    bad2.mutable_frequency_axis() = bad.frequency_axis();
    bad2.mutable_time_axis() = bad.time_axis();
    bad2.mutable_channel_info() = bad.channel_info();
    bad2.mutable_normalization_info() = bad.normalization_info();
    bad2.mutable_source_metadata() = bad.source_metadata();
    bad2.mutable_representation() = bad.representation();
    bad2.add_frame(bf);
    SpectralFrame bf1;
    bad.get_frame(1, bf1);
    bad2.add_frame(bf1);
    CHECK(bad2.validate().has_errors(), "unsupported reassignment rejected");
    // F-missing: Available phase with empty vectors is rejected.
    SpectralDataset no_phase;
    no_phase.mutable_analysis_metadata().fft_size = 8;
    no_phase.mutable_analysis_metadata().hop_size = 4;
    no_phase.mutable_analysis_metadata().sample_rate = 8000;
    no_phase.mutable_analysis_metadata().num_frequency_bins = 5;
    no_phase.mutable_frequency_axis() = FrequencyAxis(8, 8000);
    no_phase.mutable_time_axis() = TimeAxis(1, 4, 8000);
    SpectralFrame nf;
    nf.frame_index = 0;
    nf.n_fft = 8;
    nf.magnitudes.assign(5, 0.2f);
    nf.power.assign(5, 0.04f);
    // phases stays empty while representation says Available
    no_phase.add_frame(nf);
    CHECK(no_phase.validate().has_errors(), "missing phase under Available rejected");
}

int main() {
    test_enumeration();
    test_stft_metadata();
    test_non_stft_metadata();
    test_invalid_rejected();
    test_fingerprint_tracks_representation();
    test_path_independence();
    test_dataset_coherence();
    test_v3_and_identity_gates();
    std::printf("\n=== representation: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
