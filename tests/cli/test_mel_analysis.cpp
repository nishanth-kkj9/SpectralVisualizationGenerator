// tests/cli/test_mel_analysis.cpp
// Phase 9 — real Mel spectrogram representation tests. Analysis behavior
// (streaming, chunking, configs, rendering, fingerprint, serialization)
// plus numerical sanity (tone response, sweep monotonicity, silence) and
// an independent per-frame reference (old-path stft_frame + verified
// filterbank) so production is never tested only against itself.
#include "media_decoder.h"
#include "mel_filterbank.h"
#include "mel_renderer.h"
#include "pipeline.h"
#include "stft.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static int g_run = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        ++g_run;                                                    \
        if (cond) {                                                 \
            ++g_pass;                                               \
        } else {                                                    \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);   \
        }                                                           \
    } while (0)

static fs::path g_dir;

static void write_wav_samples(const fs::path& p, int sr, const std::vector<float>& mono) {
    const int n = static_cast<int>(mono.size());
    std::ofstream f(p, std::ios::binary);
    const int data_size = n * 2, chunk = 36 + data_size;
    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&chunk), 4);
    f.write("WAVEfmt ", 8);
    const int fmt = 16;
    f.write(reinterpret_cast<const char*>(&fmt), 4);
    const int16_t tag = 1, ch = 1;
    f.write(reinterpret_cast<const char*>(&tag), 2);
    f.write(reinterpret_cast<const char*>(&ch), 2);
    f.write(reinterpret_cast<const char*>(&sr), 4);
    const int br = sr * 2;
    f.write(reinterpret_cast<const char*>(&br), 4);
    const int16_t ba = 2, bps = 16;
    f.write(reinterpret_cast<const char*>(&ba), 2);
    f.write(reinterpret_cast<const char*>(&bps), 2);
    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&data_size), 4);
    for (int i = 0; i < n; ++i) {
        const float v = mono[static_cast<size_t>(i)];
        const auto s =
            static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f);
        f.write(reinterpret_cast<const char*>(&s), 2);
    }
}

static std::vector<float> sine_mix(int n, int sr) {
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        out[static_cast<size_t>(i)] =
            0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / sr) +
            0.25f * std::sin(2.0f * 3.14159265f * 1320.0f * i / sr);
    return out;
}

static Spectral::GenerateConfig mel_cfg(const std::string& in, const std::string& out,
                                        int fft = 1024, int hop = 256,
                                        int bands = 64,
                                        const std::string& norm = "slaney") {
    Spectral::GenerateConfig cfg;
    cfg.input_path = in;
    cfg.output_path = out;
    cfg.fft_size = fft;
    cfg.hop_size = hop;
    cfg.width = 256;
    cfg.height = 128;
    cfg.representation = "mel";
    cfg.mel_bands = bands;
    cfg.mel_norm = norm;
    return cfg;
}

static int argmax_band(const Spectral::SpectralFrame& fr) {
    int best = 0;
    for (size_t k = 1; k < fr.magnitudes.size(); ++k)
        if (fr.magnitudes[k] > fr.magnitudes[best]) best = static_cast<int>(k);
    return best;
}

// --- representation metadata --------------------------------------------------
static void test_metadata() {
    std::printf("[metadata]\n");
    write_wav_samples(g_dir / "m.wav", 22050, sine_mix(22050 * 5, 22050));
    auto cfg = mel_cfg((g_dir / "m.wav").string(), (g_dir / "x.png").string());
    Spectral::SpectralDataset ds;
    CHECK(Spectral::analyze_dataset(cfg, ds).ok(), "mel analyzes");
    {
        auto vr = ds.validate();
        if (!vr.valid)
            for (auto& e : vr.errors) std::printf("  DIAG melval: %s\n", e.c_str());
        CHECK(vr.valid, "mel dataset validates TEMP");
    }
    CHECK(ds.representation().kind == Spectral::RepresentationKind::Mel, "kind mel");
    CHECK(ds.representation().bins == 64, "explicit bins");
    CHECK(ds.num_frequency_bins() == 64, "axis bins");
    CHECK(ds.representation().bin_centers.size() == 64, "explicit centers");
    CHECK(ds.representation().bin_centers == ds.frequency_axis().bin_frequencies,
          "centers authoritative");
    CHECK(ds.representation().phase == Spectral::RepresentationPhase::NotApplicable,
          "phase N/A");
    CHECK(!ds.representation().reassignment_supported, "no reassignment");
    for (int f = 0; f < ds.frame_count(); ++f) {
        CHECK(ds.frame(f).phases.empty(), "frames phaseless");
        CHECK(ds.frame(f).reassigned_times.empty(), "frames without reassign");
        CHECK(static_cast<int>(ds.frame(f).magnitudes.size()) == 64, "frame width");
    }
    CHECK(ds.analysis_metadata().analysis_method == "mel", "method mel");
    CHECK(ds.analysis_metadata().analyzer_version == "1", "mel version 1");
    CHECK(ds.analysis_metadata().num_frequency_bins == 64, "analysis bins");
    CHECK(ds.analysis_metadata().fft_size == 1024, "fft kept as impl param");
    CHECK(ds.validate().valid, "mel dataset validates");
    // Ordered nonuniform centers (log-spaced Mel is not uniform in Hz).
    bool ordered = true, nonuniform = false;
    const auto& c = ds.representation().bin_centers;
    for (size_t i = 1; i < c.size(); ++i) {
        if (!(c[i] > c[i - 1])) ordered = false;
        if (i > 1 && std::fabs((c[i] - c[i - 1]) - (c[1] - c[0])) > 1.0f)
            nonuniform = true;
    }
    CHECK(ordered, "centers ordered");
    CHECK(nonuniform, "centers nonuniform");
}

// --- config validation ----------------------------------------------------------
static void test_config_validation() {
    std::printf("[config_validation]\n");
    auto good =
        mel_cfg((g_dir / "m.wav").string(), (g_dir / "x.png").string());
    CHECK(Spectral::validate_config(good).empty(), "valid mel config");
    {
        auto cfg = good;
        cfg.representation = "bark";
        CHECK(!Spectral::validate_config(cfg).empty(), "bark representation rejected");
    }
    {
        auto cfg = good;
        cfg.reassigned = true;
        CHECK(!Spectral::validate_config(cfg).empty(), "reassign+mel rejected");
    }
    {
        auto cfg = good;
        cfg.mel_bands = 0;
        CHECK(!Spectral::validate_config(cfg).empty(), "zero bands rejected");
    }
    {
        auto cfg = good;
        cfg.mel_bands = 513 + 1;  // fft 1024 -> 513 bins max
        CHECK(!Spectral::validate_config(cfg).empty(), "bands>bins rejected");
    }
    {
        auto cfg = good;
        cfg.mel_norm = "bogus";
        CHECK(!Spectral::validate_config(cfg).empty(), "bad norm rejected");
    }
    {
        auto cfg = good;
        cfg.max_freq = 30000.0f;  // above 22050/2 nyquist: filterbank must fail
        // validate itself cannot know sr; the build refuses with BadConfig.
        Spectral::SpectralDataset ds;
        CHECK(Spectral::analyze_dataset(cfg, ds) == Spectral::JobError::BadConfig,
              "fmax>nyquist fails at build");
    }
}

// --- numerical sanity --------------------------------------------------------------
static void test_numerical_sanity() {
    std::printf("[numerical_sanity]\n");
    const int sr = 22050;
    // Single 440 Hz tone: strongest response near the Mel band of 440 Hz.
    std::vector<float> tone(static_cast<size_t>(sr * 3));
    for (int i = 0; i < sr * 3; ++i)
        tone[static_cast<size_t>(i)] =
            0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / sr);
    write_wav_samples(g_dir / "tone.wav", sr, tone);
    auto cfg = mel_cfg((g_dir / "tone.wav").string(), (g_dir / "x.png").string(), 2048,
                       512, 64);
    Spectral::SpectralDataset ds;
    CHECK(Spectral::analyze_dataset(cfg, ds).ok(), "tone analyzes");
    Spectral::MelFilterbank bank;
    std::string err;
    Spectral::MelFilterbankConfig bcfg;
    bcfg.sample_rate = sr;
    bcfg.fft_size = 2048;
    bcfg.bands = 64;
    CHECK(bank.build(bcfg, err), "expected bank builds");
    int expect_band = 0;
    float best = -1.0f;
    for (int b = 0; b < 64; ++b) {
        const float d = std::fabs(bank.centers_hz()[b] - 440.0f);
        if (best < 0.0f || d < best) {
            best = d;
            expect_band = b;
        }
    }
    bool near = true;
    for (int f = 0; f < ds.frame_count(); ++f) {
        if (std::abs(argmax_band(ds.frame(f)) - expect_band) > 1) {
            near = false;
            break;
        }
    }
    CHECK(near, "tone peaks at the 440Hz mel band");
    // Sweep moves the peak monotonically through bands.
    int prev_peak = -1;
    bool mono = true;
    for (float freq : {220.0f, 440.0f, 880.0f, 1760.0f, 3520.0f}) {
        std::vector<float> t(static_cast<size_t>(sr));
        for (int i = 0; i < sr; ++i)
            t[static_cast<size_t>(i)] =
                0.4f * std::sin(2.0f * 3.14159265f * freq * i / sr);
        write_wav_samples(g_dir / "sw.wav", sr, t);
        auto c2 = mel_cfg((g_dir / "sw.wav").string(), (g_dir / "x.png").string(), 2048,
                          1024, 64);
        Spectral::SpectralDataset d2;
        CHECK(Spectral::analyze_dataset(c2, d2).ok(), "sweep tone analyzes");
        const int peak = argmax_band(d2.frame(d2.frame_count() / 2));
        if (peak < prev_peak) mono = false;
        prev_peak = peak;
    }
    CHECK(mono, "sweep peak moves monotonically");
    // Silence is deterministic zeros (sqrt guarded, no NaN).
    {
        std::vector<float> silence(static_cast<size_t>(sr), 0.0f);
        write_wav_samples(g_dir / "sil.wav", sr, silence);
        auto c3 = mel_cfg((g_dir / "sil.wav").string(), (g_dir / "x.png").string());
        Spectral::SpectralDataset d3;
        CHECK(Spectral::analyze_dataset(c3, d3).ok(), "silence analyzes");
        bool ok = d3.frame_count() > 0;
        for (int f = 0; ok && f < d3.frame_count(); ++f) {
            for (float m : d3.frame(f).magnitudes)
                if (!(m == 0.0f)) {
                    ok = false;
                    break;
                }
        }
        CHECK(ok, "silence yields exact zeros");
    }
    // Stats are finite and centroid sits inside the range.
    {
        bool ok = ds.frame_count() > 0;
        for (int f = 0; ok && f < ds.frame_count(); ++f) {
            const auto& fr = ds.frame(f);
            ok = std::isfinite(fr.rms) && std::isfinite(fr.peak_magnitude) &&
                 std::isfinite(fr.spectral_centroid) &&
                 std::isfinite(fr.spectral_bandwidth) && fr.spectral_centroid >= 0.0f &&
                 fr.spectral_centroid <= 11025.0f;
        }
        CHECK(ok, "mel stats finite and in range");
    }
}

// --- amplitude domain (aggregation input contract) --------------------------------
// Mel must consume the STFT contract's one-sided amplitude-corrected power, so a
// pure tone's Mel band magnitude lives in the SAME amplitude domain as the STFT
// peak magnitude. Aggregating raw |X|^2 would inflate it by the window's N*cg
// factor (~+55 dB at N=1024, Hann) and saturate every renderer while still
// claiming neutral window gains — this test fails loudly on that regression.
static void test_amplitude_domain() {
    std::printf("[amplitude_domain]\n");
    const int sr = 22050, fft = 1024, hop = 256, bands = 64;
    const float amp = 0.5f;
    std::vector<float> tone(static_cast<size_t>(sr) * 2);
    for (int i = 0; i < sr * 2; ++i)
        tone[static_cast<size_t>(i)] =
            amp * std::sin(2.0f * 3.14159265f * 440.0f * i / sr);
    write_wav_samples(g_dir / "amp.wav", sr, tone);

    // Mel, norm=none: unit-peak triangles, so the band holding the tone
    // reports ~the tone's amplitude (a small number of neighbouring bins).
    auto mcfg = mel_cfg((g_dir / "amp.wav").string(), (g_dir / "x.png").string(),
                        fft, hop, bands, "none");
    Spectral::SpectralDataset mel;
    CHECK(Spectral::analyze_dataset(mcfg, mel).ok(), "amplitude mel analyzes");
    float mel_peak = 0.0f;
    for (int f = 0; f < mel.frame_count(); ++f)
        mel_peak = std::max(mel_peak, mel.frame(f).peak_magnitude);

    // The identical signal through the STFT representation (unchanged path).
    auto scfg = mcfg;
    scfg.representation = "stft";
    Spectral::SpectralDataset stft;
    CHECK(Spectral::analyze_dataset(scfg, stft).ok(), "amplitude stft analyzes");
    float stft_peak = 0.0f;
    for (int f = 0; f < stft.frame_count(); ++f)
        stft_peak = std::max(stft_peak, stft.frame(f).peak_magnitude);

    CHECK(mel_peak > 0.0f && stft_peak > 0.0f, "both peaks measurable");
    const float ratio = (stft_peak > 0.0f) ? mel_peak / stft_peak : 0.0f;
    CHECK(ratio > 0.4f && ratio < 3.0f,
          "mel peak stays in the STFT amplitude domain");
    // Raw-power aggregation would land near fft*cg (~512) instead.
    CHECK(mel_peak < 5.0f, "mel magnitude carries no fft*cg factor");
}

// --- independent per-frame reference -----------------------------------------------
// Full decode in-test, old-path stft_frame per frame, production Mel bank
// for aggregation (the bank itself is proven against its own reference).
// This isolates the pipeline's feeding/stats/metadata from the transform.
static void test_frame_reference() {
    std::printf("[frame_reference]\n");
    const int sr = 22050, fft = 1024, hop = 256, bands = 64;
    MediaDecoder dec;
    CHECK(dec.open((g_dir / "m.wav").string()), "reference opens fixture");
    std::vector<float> mono;
    AudioFrame fr;
    while (dec.read_frame(fr)) mono.insert(mono.end(), fr.samples.begin(), fr.samples.end());
    CHECK(!dec.failed(), "reference decodes");
    auto cfg = mel_cfg((g_dir / "m.wav").string(), (g_dir / "x.png").string(), fft, hop,
                       bands);
    Spectral::SpectralDataset ds;
    CHECK(Spectral::analyze_dataset(cfg, ds).ok(), "streaming analyzes");
    const auto win = Spectral::stft_window("hann", fft);
    const float cg = window_coherent_gain(win);
    Spectral::MelFilterbank bank;
    std::string err;
    Spectral::MelFilterbankConfig bcfg;
    bcfg.sample_rate = sr;
    bcfg.fft_size = fft;
    bcfg.bands = bands;
    CHECK(bank.build(bcfg, err), "reference bank builds");
    const int count = Spectral::stft_frame_count(static_cast<int>(mono.size()), fft, hop);
    CHECK(ds.frame_count() == count, "frame counts agree");
    std::vector<float> power(fft / 2 + 1), energy(bands);
    bool ok = ds.frame_count() == count;
    for (int f = 0; ok && f < count; ++f) {
        Spectral::StftFrame ref;
        if (!Spectral::stft_frame(mono.data(), static_cast<int>(mono.size()), f * hop,
                                  fft, sr, win, cg, ref)) {
            ok = false;
            break;
        }
        // Aggregation input: the STFT contract's one-sided
        // amplitude-corrected power — interior bins doubled, DC/Nyquist
        // single, scale 1/(N*cg) — re-derived here in the test's own code
        // (same documented definition, not a call into production).
        const float norm = 1.0f / (static_cast<float>(fft) * cg);
        for (int k = 0; k <= fft / 2; ++k) {
            const float re = ref.spectrum[k].real(), im = ref.spectrum[k].imag();
            float mag = std::sqrt(re * re + im * im) * norm;
            if (k > 0 && k < fft / 2) mag *= 2.0f;
            power[k] = mag * mag;
        }
        bank.apply(power.data(), energy.data());
        const auto& got = ds.frame(f);
        if (got.timestamp != f * hop / static_cast<double>(sr)) {
            ok = false;
            break;
        }
        for (int b = 0; ok && b < bands; ++b) {
            // Both paths aggregate identical power through identical
            // weights; magnitudes may differ by one sqrt rounding
            // (tolerance therefore scales with the band energy).
            if (std::fabs(got.power[b] - energy[b]) > 1e-5f * (1.0f + energy[b]))
                ok = false;
            const float expect_mag =
                energy[b] > 0.0f ? std::sqrt(energy[b]) : 0.0f;
            if (std::fabs(got.magnitudes[b] - expect_mag) >
                1e-6f * (1.0f + energy[b]))
                ok = false;
        }
    }
    CHECK(ok, "streaming mel matches independent frame reference");
}

// --- matrix: configs, chunking, norms -------------------------------------------------
static void test_matrix() {
    std::printf("[matrix]\n");
    // Stereo + rates + windows + bands + ranges + norms.
    {
        const int sr = 22050, n = sr * 5;
        std::vector<float> l(n), r(n);
        for (int i = 0; i < n; ++i) {
            l[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / sr);
            r[i] = 0.25f * std::sin(2.0f * 3.14159265f * 880.0f * i / sr);
        }
        // stereo writer (interleaved)
        std::ofstream f((g_dir / "stm.wav").string(), std::ios::binary);
        const int data_size = n * 4, chunk = 36 + data_size;
        f.write("RIFF", 4);
        f.write(reinterpret_cast<const char*>(&chunk), 4);
        f.write("WAVEfmt ", 8);
        const int fmt = 16;
        f.write(reinterpret_cast<const char*>(&fmt), 4);
        const int16_t tag = 1, ch = 2;
        f.write(reinterpret_cast<const char*>(&tag), 2);
        f.write(reinterpret_cast<const char*>(&ch), 2);
        f.write(reinterpret_cast<const char*>(&sr), 4);
        const int br = sr * 4;
        f.write(reinterpret_cast<const char*>(&br), 4);
        const int16_t ba = 4, bps = 16;
        f.write(reinterpret_cast<const char*>(&ba), 2);
        f.write(reinterpret_cast<const char*>(&bps), 2);
        f.write("data", 4);
        f.write(reinterpret_cast<const char*>(&data_size), 4);
        for (int i = 0; i < n; ++i)
            for (float v : {l[i], r[i]}) {
                const auto s = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, v)) *
                                                    32767.0f);
                f.write(reinterpret_cast<const char*>(&s), 2);
            }
        f.close();
        auto cfg = mel_cfg((g_dir / "stm.wav").string(), (g_dir / "x.png").string());
        Spectral::SpectralDataset ds;
        CHECK(Spectral::analyze_dataset(cfg, ds).ok(), "stereo mel analyzes");
        CHECK(ds.validate().valid, "stereo mel validates");
        CHECK(ds.channel_info().channels_mixed, "stereo marked mixed");
    }
    for (int sr : {8000, 44100}) {
        write_wav_samples(g_dir / "sr.wav", sr, sine_mix(sr * 3, sr));
        auto cfg = mel_cfg((g_dir / "sr.wav").string(), (g_dir / "x.png").string(), 512,
                           128, 32);
        Spectral::SpectralDataset ds;
        CHECK(Spectral::analyze_dataset(cfg, ds).ok(), "rate analyzes");
        CHECK(ds.validate().valid, "rate validates");
    }
    for (const char* w : {"hann", "hamming", "blackman", "rectangular"}) {
        auto cfg = mel_cfg((g_dir / "m.wav").string(), (g_dir / "x.png").string());
        cfg.window = w;
        Spectral::SpectralDataset ds;
        CHECK(Spectral::analyze_dataset(cfg, ds).ok(), "window analyzes");
        CHECK(ds.validate().valid, "window validates");
    }
    for (int bands : {16, 64, 128}) {
        auto cfg = mel_cfg((g_dir / "m.wav").string(), (g_dir / "x.png").string(), 1024,
                           256, bands);
        Spectral::SpectralDataset ds;
        CHECK(Spectral::analyze_dataset(cfg, ds).ok(), "bands analyzes");
        CHECK(ds.num_frequency_bins() == bands, "band count honored");
        CHECK(ds.validate().valid, "bands validates");
    }
    {
        // Explicit range + all norms validate and differ from each other.
        Spectral::SpectralDataset mods[3];
        const char* norms[3] = {"none", "slaney", "area"};
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            auto cfg = mel_cfg((g_dir / "m.wav").string(), (g_dir / "x.png").string(),
                               1024, 256, 48, norms[i]);
            cfg.min_freq = 100.0f;
            cfg.max_freq = 8000.0f;
            if (!Spectral::analyze_dataset(cfg, mods[i]).ok()) ok = false;
            if (!mods[i].validate().valid) ok = false;
        }
        CHECK(ok, "ranges+norms analyze and validate");
        CHECK(mods[0].representation().fmin_hz == 100.0f, "explicit fmin kept");
        const bool differ =
            mods[0].frame(0).magnitudes != mods[1].frame(0).magnitudes &&
            mods[1].frame(0).magnitudes != mods[2].frame(0).magnitudes;
        CHECK(differ, "norms produce different energies");
    }
    {
        // Chunk invariance (tiny/default/prime/large).
        auto base = mel_cfg((g_dir / "m.wav").string(), (g_dir / "x.png").string());
        Spectral::SpectralDataset a, b;
        CHECK(Spectral::analyze_dataset(base, a).ok(), "default chunks ok");
        CHECK(Spectral::analyze_dataset(base, b, {}, nullptr, 371).ok(),
              "prime chunks ok");
        bool same = a.frame_count() == b.frame_count();
        for (int f = 0; same && f < a.frame_count(); ++f) {
            const auto& fa = a.frame(f);
            const auto& fb = b.frame(f);
            same = fa.magnitudes == fb.magnitudes && fa.power == fb.power &&
                   fa.timestamp == fb.timestamp && fa.rms == fb.rms &&
                   fa.spectral_centroid == fb.spectral_centroid;
        }
        CHECK(same, "mel chunk-invariant");
    }
    {
        // Exact-FFT input and too-short input.
        write_wav_samples(g_dir / "tiny.wav", 22050,
                          std::vector<float>(1024, 0.1f));
        Spectral::SpectralDataset ds;
        auto cfg = mel_cfg((g_dir / "tiny.wav").string(), (g_dir / "x.png").string(),
                           1024, 512, 32);
        CHECK(Spectral::analyze_dataset(cfg, ds).ok() && ds.frame_count() == 1,
              "exact-fft mel gives 1 frame");
        write_wav_samples(g_dir / "tooshort.wav", 22050, std::vector<float>(100, 0.1f));
        Spectral::SpectralDataset ds2;
        auto cfg2 = mel_cfg((g_dir / "tooshort.wav").string(),
                            (g_dir / "x.png").string(), 1024, 512, 32);
        CHECK(Spectral::analyze_dataset(cfg2, ds2) == Spectral::JobError::AnalysisError,
              "short mel input rejected");
    }
}

// --- temporal column mapping --------------------------------------------------------
// Build a synthetic Mel dataset whose frames are trivially distinguishable:
// frame f has every band set to (f+1)*0.2 so first != last. Rendering must
// follow the STFT TimeMapper convention: column x covers time
// t = x/(W-1) * total_duration and shows the frame nearest that time
// (frames sit at their real timestamps beginning at 0). So the first column
// is frame 0, the last column is the final frame, and interior columns land
// on the nearest real timestamp -- not on an index stretch. The previously
// committed x/W mapping put the last column at ~round((Nf-1)/2) for W=2, and
// index stretching put W=4 column 1 on frame 1 instead of frame 2.
static Spectral::SpectralDataset make_temporal_mel(int bands, int frames) {
    Spectral::SpectralDataset d;
    std::vector<float> centers(static_cast<size_t>(bands));
    for (int b = 0; b < bands; ++b)
        centers[static_cast<size_t>(b)] = 100.0f + 200.0f * b;
    d.mutable_frequency_axis() = Spectral::FrequencyAxis::from_centers(centers, 22050);
    auto& am = d.mutable_analysis_metadata();
    am.fft_size = 1024;
    am.hop_size = 256;
    am.sample_rate = 22050;
    am.num_frequency_bins = bands;
    am.nyquist_frequency = 11025.0f;
    auto& rep = d.mutable_representation();
    rep.kind = Spectral::RepresentationKind::Mel;
    rep.bins = bands;
    rep.fmin_hz = centers.front();
    rep.fmax_hz = centers.back();
    rep.bands = bands;
    rep.norm = Spectral::RepresentationNorm::None;
    rep.phase = Spectral::RepresentationPhase::NotApplicable;
    rep.reassignment_supported = false;
    rep.bin_centers = centers;
    d.mutable_time_axis() = Spectral::TimeAxis(frames, 256, 22050);
    for (int f = 0; f < frames; ++f) {
        Spectral::SpectralFrame fr;
        fr.frame_index = f;
        fr.n_fft = 0;
        fr.timestamp = static_cast<double>(f * 256) / 22050;
        fr.magnitudes.assign(static_cast<size_t>(bands), 0.2f * (f + 1));
        fr.power.assign(static_cast<size_t>(bands), 0.0f);
        for (int k = 0; k < bands; ++k)
            fr.power[static_cast<size_t>(k)] =
                fr.magnitudes[static_cast<size_t>(k)] *
                fr.magnitudes[static_cast<size_t>(k)];
        d.add_frame(fr);
    }
    return d;
}

static void column_pixels(const Spectral::RGBAImage& img, int x, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(static_cast<size_t>(img.height) * 4);
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* p =
            &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
        out.insert(out.end(), p, p + 4);
    }
}

static void test_temporal_mapping() {
    std::printf("[temporal_mapping]\n");
    const int bands = 8, frames = 5, height = 4;
    Spectral::SpectralDataset ds = make_temporal_mel(bands, frames);
    CHECK(ds.validate().valid, "temporal fixture validates");
    auto render_w = [&](int w, Spectral::RGBAImage& img) {
        Spectral::MelSpectrogramConfig mc;
        mc.width = w;
        mc.height = height;
        Spectral::MelSpectrogramRenderer r(mc);
        return r.render(ds, img);
    };
    // Single 96x192 expected pixel for (frame, band): mirrors the renderer
    // chain (20*log10 mag + normalize_db + viridis) without reusing its
    // column selection, so a wrong column cannot hide.
    auto expect_px = [&](int frame, int band) {
        const float mag = 0.2f * (frame + 1);
        const float ref = ds.normalization_info().reference_amplitude;
        const float db = 20.0f * std::log10(mag / ref);
        const float t = Spectral::SpectrogramRenderer::normalize_db(
            db, -90.0f, 0.0f);
        uint8_t r = 0, g = 0, b = 0;
        Spectral::SpectrogramRenderer::color_map(Spectral::ColorMap::Viridis,
                                                t, r, g, b);
        return std::array<uint8_t, 4>{r, g, b, 255};
    };
    auto col_matches_frame = [&](const Spectral::RGBAImage& img, int x,
                                 int frame) {
        for (int y = 0; y < height; ++y) {
            const int band = bands - 1 -
                static_cast<int>(std::llround(static_cast<double>(y) /
                                              (height - 1) * (bands - 1)));
            const auto e = expect_px(frame, band);
            const uint8_t* p =
                &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
            for (int c = 0; c < 4; ++c)
                if (p[c] != e[static_cast<size_t>(c)]) return false;
        }
        return true;
    };
    {
        Spectral::RGBAImage img;
        CHECK(render_w(1, img) == Spectral::MelRenderError::Ok,
              "temporal W=1 renders");
        CHECK(img.width == 1 && img.height == height, "temporal W=1 dims");
        CHECK(col_matches_frame(img, 0, 0), "W=1 shows the first frame");
    }
    for (int w : {2, 4, 16}) {
        Spectral::RGBAImage img;
        CHECK(render_w(w, img) == Spectral::MelRenderError::Ok,
              "temporal renders");
        CHECK(img.width == w && img.height == height, "temporal dims");
        CHECK(col_matches_frame(img, 0, 0), "first column is frame 0");
        CHECK(col_matches_frame(img, w - 1, frames - 1),
              "last column is the final frame");
        // Whole-column sanity: the old x/W build maps the W=2 last column
        // to frame ~1, which this pixel comparison rejects.
        std::vector<uint8_t> first, last;
        column_pixels(img, 0, first);
        column_pixels(img, w - 1, last);
        CHECK(first != last, "endpoints distinguishable");
        CHECK(!col_matches_frame(img, w - 1, 0),
              "last column is not the first frame");
    }
    {
        // Interior columns follow the real timestamps: W=4 over 5 frames
        // spans times {0, 1/3, 2/3, 1} * total_duration, whose nearest frames
        // are {0, 2, 3, 4}. Index stretching would give {0, 1, 3, 4}.
        Spectral::RGBAImage img;
        CHECK(render_w(4, img) == Spectral::MelRenderError::Ok,
              "interior mapping renders");
        const int expected[4] = {0, 2, 3, 4};
        for (int x = 0; x < 4; ++x)
            CHECK(col_matches_frame(img, x, expected[x]),
                  "column shows the frame nearest its time");
    }
}

// --- rendering + fingerprint + serialization ----------------------------------------------
static void test_render_fingerprint_serial() {
    std::printf("[render_fingerprint_serial]\n");
    auto cfg = mel_cfg((g_dir / "m.wav").string(), (g_dir / "mel.png").string(), 1024,
                       256, 64);
    fs::remove(g_dir / "mel.png");
    CHECK(Spectral::run_job(cfg) == Spectral::JobError::Ok, "mel image job ok");
    CHECK(fs::exists(g_dir / "mel.png"), "mel image produced");
    {
        // Spectrum visualization stays STFT-only: clear typed refusal.
        auto c2 = cfg;
        c2.visualization = "spectrum";
        c2.output_path = (g_dir / "mel_spec.png").string();
        CHECK(Spectral::run_job(c2) == Spectral::JobError::RenderError,
              "mel spectrum refused as RenderError");
    }
    {
        // Video path renders Mel frames (ffmpeg-gated like other video tests).
        auto c3 = cfg;
        c3.output_format = "video";
        c3.output_format_explicit = true;
        c3.output_path = (g_dir / "mel.mp4").string();
        c3.width = 128;
        c3.height = 64;
        c3.fps = 10;
        fs::remove(g_dir / "mel.mp4");
        auto err = Spectral::run_job(c3);
        if (err == Spectral::JobError::DependencyMissing) {
            std::printf("  (ffmpeg missing: mel video skipped)\n");
        } else {
            CHECK(err == Spectral::JobError::Ok, "mel video job ok");
            CHECK(fs::exists(g_dir / "mel.mp4"), "mel video produced");
        }
    }
    {
        // Single-row/single-column edge geometry (H==1 divides by (H-1) in
        // the naive row map): every positive size must render safely with a
        // deterministic result, never NaN/UB.
        Spectral::SpectralDataset ds;
        CHECK(Spectral::analyze_dataset(cfg, ds).ok(), "edge geometry analyzes");
        CHECK(ds.frame_count() > 0, "edge geometry has frames");
        auto edge_ok = [&](int w, int h) {
            Spectral::MelSpectrogramConfig mc;
            mc.width = w;
            mc.height = h;
            Spectral::MelSpectrogramRenderer r(mc);
            Spectral::RGBAImage img;
            if (r.render(ds, img) != Spectral::MelRenderError::Ok) return false;
            if (img.width != w || img.height != h) return false;
            if (img.pixels.size() != static_cast<size_t>(w) * h * 4) return false;
            for (float v : ds.frame(0).magnitudes)
                if (!std::isfinite(v)) return false;
            return img.valid();
        };
        CHECK(edge_ok(1, 1), "mel renders 1x1");
        CHECK(edge_ok(1, 64), "mel renders 1xN");
        CHECK(edge_ok(64, 1), "mel renders Nx1");
        CHECK(edge_ok(64, 32), "mel renders normal dims");
        {
            // Single row selects the deterministic middle band for every
            // column: the same band the production single-row path uses.
            Spectral::MelSpectrogramConfig mc;
            mc.width = 4;
            mc.height = 1;
            Spectral::MelSpectrogramRenderer r(mc);
            Spectral::RGBAImage img;
            CHECK(r.render(ds, img) == Spectral::MelRenderError::Ok,
                  "Nx1 direct render ok");
            const int nb = ds.num_frequency_bins();
            const int mid = (nb - 1) / 2;
            const float mag = ds.frame(0).magnitudes[static_cast<size_t>(mid)];
            const float ref = ds.normalization_info().reference_amplitude > 0.0f
                                  ? ds.normalization_info().reference_amplitude
                                  : mc.reference_amplitude;
            const float db = (!(mag > 0.0f) || !(ref > 0.0f))
                                 ? mc.db_floor
                                 : 20.0f * std::log10(mag / ref);
            const float t = Spectral::SpectrogramRenderer::normalize_db(
                db, mc.db_floor, mc.db_ceiling);
            uint8_t er = 0, eg = 0, eb = 0;
            Spectral::SpectrogramRenderer::color_map(mc.color_map, t, er, eg, eb);
            bool match = true;
            for (int x = 0; x < 4; ++x) {
                const uint8_t* p = &img.pixels[static_cast<size_t>(x) * 4];
                if (p[0] != er || p[1] != eg || p[2] != eb || p[3] != 255) {
                    match = false;
                    break;
                }
            }
            CHECK(match, "Nx1 row is the deterministic middle band");
        }
        {
            // The same tiny geometry through the normal run_job() image path.
            auto tiny = cfg;
            tiny.width = 1;
            tiny.height = 1;
            tiny.output_path = (g_dir / "mel_tiny.png").string();
            fs::remove(tiny.output_path);
            CHECK(Spectral::run_job(tiny) == Spectral::JobError::Ok,
                  "1x1 image job ok");
            CHECK(fs::exists(tiny.output_path), "1x1 image produced");
            auto wide = cfg;
            wide.width = 64;
            wide.height = 1;
            wide.output_path = (g_dir / "mel_wide1.png").string();
            fs::remove(wide.output_path);
            CHECK(Spectral::run_job(wide) == Spectral::JobError::Ok,
                  "Nx1 image job ok");
            CHECK(fs::exists(wide.output_path), "Nx1 image produced");
        }
    }
    {
        // Fingerprint: mel-defining params move it, renderer-only does not.
        Spectral::DecodedMedia media;
        media.file_path = "x";
        media.file_hash = std::string(64, 'a');
        media.file_size_bytes = 100;
        media.sample_rate = 22050;
        media.num_channels = 1;
        media.duration_seconds = 5.0;
        Spectral::GenerateConfig base;
        base.input_path = "x";
        base.output_path = "y.png";
        base.representation = "mel";
        const std::string f0 = Spectral::make_project_config(base, media).analysis_fingerprint();
        base.mel_bands = 128;
        const std::string f1 = Spectral::make_project_config(base, media).analysis_fingerprint();
        CHECK(!f0.empty() && f0 != f1, "band count moves fingerprint");
        base.mel_bands = 64;
        base.mel_norm = "area";
        const std::string f2 = Spectral::make_project_config(base, media).analysis_fingerprint();
        CHECK(f2 != f0, "norm moves fingerprint");
        base.mel_norm = "slaney";
        base.min_freq = 100.0f;
        const std::string f3 = Spectral::make_project_config(base, media).analysis_fingerprint();
        CHECK(f3 != f0, "range moves fingerprint");
        base.min_freq = 0.0f;
        base.width = 640;  // renderer-only
        const std::string f4 = Spectral::make_project_config(base, media).analysis_fingerprint();
        CHECK(f4 == f0, "renderer change keeps fingerprint");
        base.representation = "stft";
        const std::string f5 = Spectral::make_project_config(base, media).analysis_fingerprint();
        CHECK(f5 != f0, "representation moves fingerprint");
    }
    {
        // JSON round-trips a real Mel analysis as Mel.
        Spectral::SpectralDataset ds;
        CHECK(Spectral::analyze_dataset(cfg, ds).ok(), "mel analyzes for json");
        Spectral::SpectralDataset loaded;
        CHECK(loaded.deserialize_json(ds.serialize_json()), "mel json loads");
        CHECK(loaded.validate().valid, "loaded mel validates");
        CHECK(loaded.representation() == ds.representation(), "mel rep round-trips");
        CHECK(loaded.analysis_metadata().analysis_method == "mel", "method round-trips");
        CHECK(loaded == ds, "mel json round-trips");
    }
}

// --- memory + cancel --------------------------------------------------------------------------
static void test_memory_cancel() {
    std::printf("[memory_cancel]\n");
    write_wav_samples(g_dir / "bigm.wav", 22050, sine_mix(22050 * 60, 22050));
    {
        Spectral::SpectralDataset ds;
        Spectral::AnalyzeStats stats;
        auto cfg = mel_cfg((g_dir / "bigm.wav").string(), (g_dir / "x.png").string(),
                           1024, 256, 64);
        CHECK(Spectral::analyze_dataset(cfg, ds, {}, nullptr, 0, &stats).ok(),
              "60s mel analyzes");
        CHECK(stats.peak_buffered_samples <= static_cast<size_t>(2 * 1024 + 65536),
              "mel peak bounded");
        CHECK(stats.peak_buffered_samples * 4 < static_cast<size_t>(stats.decoded_samples),
              "mel peak independent of duration");
    }
    {
        const fs::path out = g_dir / "mcancel.png";
        fs::remove(out);
        std::atomic<bool> flag{false};
        Spectral::Error result = Spectral::Error::success();
        std::thread worker([&] {
            auto cfg = mel_cfg((g_dir / "bigm.wav").string(), out.string(), 2048, 512,
                               64);
            result = Spectral::run_job(
                cfg,
                [&](float f, const char* stage) {
                    if (std::string(stage) == "analyze" && f > 0.05) flag.store(true);
                },
                &flag);
        });
        worker.join();
        CHECK(result == Spectral::JobError::Cancelled, "mel cancel -> Cancelled");
        CHECK(!fs::exists(out), "no partial mel output after cancel");
    }
}

int main() {
    std::error_code ec;
    g_dir = fs::temp_directory_path(ec) / "svg_phase9_test";
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    if (ec) {
        std::printf("FAIL: cannot create test dir\n");
        return 1;
    }
    test_metadata();
    test_config_validation();
    test_numerical_sanity();
    test_amplitude_domain();
    test_frame_reference();
    test_matrix();
    test_temporal_mapping();
    test_render_fingerprint_serial();
    test_memory_cancel();
    fs::remove_all(g_dir, ec);
    std::printf("\n=== mel_analysis: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
