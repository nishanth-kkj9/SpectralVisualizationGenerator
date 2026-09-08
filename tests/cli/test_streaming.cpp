// tests/cli/test_streaming.cpp
// Phase 6 — bounded streaming analysis. Proves: the overlap buffer keeps
// identical frame contents across arbitrary decoder chunkings (bit-exact
// vs reference slices); the streaming pipeline matches an independent
// full-decode + stft_all reference exactly; raw-audio memory stays bounded
// while duration scales; frame counts/timestamps/mixing/edges match the
// long-standing policy; cancellation still aborts mid-stream.
#include "media_decoder.h"
#include "pipeline.h"
#include "stft.h"
#include "streaming_analyzer.h"

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
    const int16_t tag = 1;
    const int16_t ch = 1;
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

static void write_stereo_wav(const fs::path& p, int sr, const std::vector<float>& left,
                             const std::vector<float>& right) {
    const int n = static_cast<int>(left.size());
    std::ofstream f(p, std::ios::binary);
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
    for (int i = 0; i < n; ++i) {
        for (float v : {left[static_cast<size_t>(i)], right[static_cast<size_t>(i)]}) {
            const auto s = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, v)) *
                                                32767.0f);
            f.write(reinterpret_cast<const char*>(&s), 2);
        }
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

static Spectral::GenerateConfig job_cfg(const std::string& in, const std::string& out,
                                        int fft, int hop) {
    Spectral::GenerateConfig cfg;
    cfg.input_path = in;
    cfg.output_path = out;
    cfg.fft_size = fft;
    cfg.hop_size = hop;
    cfg.width = 256;
    cfg.height = 128;
    return cfg;
}

// --- 1. buffer unit: chunking invariance + bounds ------------------------------
static void test_buffer_chunking() {
    std::printf("[buffer_chunking]\n");
    CHECK(!Spectral::StreamingMonoBuffer(1024, 2048).valid(), "hop>fft invalid");
    CHECK(!Spectral::StreamingMonoBuffer(0, 0).valid(), "zero invalid");
    const int total = 100000;
    std::vector<float> sig(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i)
        sig[static_cast<size_t>(i)] = static_cast<float>((i * 2654435761u) % 1000) / 1000.0f;
    for (int fft : {256, 512, 1024}) {
        for (int hop : {fft / 2, fft / 4}) {
            for (size_t chunk : {1, 63, 511, 512, 4096, 100000}) {
                Spectral::StreamingMonoBuffer b(fft, hop);
                CHECK(b.valid(), "buffer valid");
                size_t fed = 0;
                int frames = 0;
                bool exact = true;
                while (fed < sig.size()) {
                    const size_t n = std::min(chunk, sig.size() - fed);
                    b.append(sig.data() + fed, n);
                    fed += n;
                    const float* span = nullptr;
                    int64_t abs_start = -1;
                    while (b.pop_frame(span, abs_start)) {
                        const int64_t f = frames++;
                        // Frame f must equal reference slice [f*hop, +fft).
                        for (int j = 0; j < fft; ++j) {
                            if (span[j] != sig[static_cast<size_t>(f * hop + j)]) {
                                exact = false;
                                break;
                            }
                        }
                        if (abs_start != f * hop) exact = false;
                        if (!exact) break;
                    }
                    if (!exact) break;
                }
                const int expect = Spectral::stft_frame_count(total, fft, hop);
                CHECK(frames == expect, "frame count matches policy");
                CHECK(exact, "frames bit-exact across chunkings");
                // Bounded: peak far below the 100k-sample signal.
                CHECK(b.peak_buffered() <= static_cast<size_t>(2 * fft + chunk),
                      "peak bounded by 2*fft+chunk");
            }
        }
    }
}

// --- reference: full decode + stft_all in-test ---------------------------------
struct Reference {
    std::vector<Spectral::StftFrame> frames;
    std::vector<float> mono;
    int sr = 0;
};

static bool decode_reference(const std::string& path, Reference& ref, size_t chunk = 0) {
    MediaDecoder dec;
    MediaDecoder::DecodeOptions opts;
    if (chunk > 0) opts.chunk_frames = chunk;
    if (!dec.open(path, opts)) return false;
    ref.sr = dec.sample_rate();
    AudioFrame fr;
    while (dec.read_frame(fr)) {
        if (fr.num_channels == 1) {
            ref.mono.insert(ref.mono.end(), fr.samples.begin(), fr.samples.end());
        } else {
            const size_t n = fr.samples.size() / static_cast<size_t>(fr.num_channels);
            for (size_t i = 0; i < n; ++i) {
                float sum = 0.0f;
                for (int ch = 0; ch < fr.num_channels; ++ch)
                    sum += fr.samples[i * static_cast<size_t>(fr.num_channels) + ch];
                ref.mono.push_back(sum / static_cast<float>(fr.num_channels));
            }
        }
    }
    return !dec.failed();
}

static bool frames_exact(const Spectral::SpectralDataset& ds, const Reference& ref,
                         int fft, int hop, const std::string& window) {
    const auto win = Spectral::stft_window(window, fft);
    const float cg = window_coherent_gain(win);
    const int count = Spectral::stft_frame_count(static_cast<int>(ref.mono.size()), fft,
                                                 hop);
    if (ds.frame_count() != count) return false;
    for (int f = 0; f < count; ++f) {
        Spectral::StftFrame sf;
        sf.frame_index = f;
        if (!Spectral::stft_frame(ref.mono.data(), static_cast<int>(ref.mono.size()),
                                  f * hop, fft, ref.sr, win, cg, sf))
            return false;
        const auto& got = ds.frame(f);
        if (got.frame_index != f) return false;
        if (got.timestamp != f * hop / static_cast<double>(ref.sr)) return false;
        if (got.magnitudes.size() != sf.magnitudes.size()) return false;
        for (size_t k = 0; k < sf.magnitudes.size(); ++k) {
            if (got.magnitudes[k] != sf.magnitudes[k]) return false;
            if (got.phases[k] != sf.phases[k]) return false;
            if (got.power[k] != sf.power[k]) return false;
        }
        // Stats recomputed independently from the reference magnitudes.
        float ss = 0.0f, pk = 0.0f, cn = 0.0f, cd = 0.0f;
        for (size_t k = 0; k < sf.magnitudes.size(); ++k) {
            const float m = sf.magnitudes[k];
            ss += m * m;
            if (m > pk) pk = m;
            cn += static_cast<float>(k) * ref.sr / fft * m;
            cd += m;
        }
        const float rms = std::sqrt(ss / static_cast<float>(sf.magnitudes.size()));
        if (got.rms != rms || got.peak_magnitude != pk) return false;
        const float cent = (cd > 0.0f) ? cn / cd : 0.0f;
        if (got.spectral_centroid != cent) return false;
    }
    return true;
}

// --- 2. streaming == reference across configs ------------------------------------
static void test_equivalence() {
    std::printf("[equivalence]\n");
    const int sr = 22050;
    write_wav_samples(g_dir / "eq.wav", sr, sine_mix(sr * 10, sr));
    for (int fft : {256, 512}) {
        for (int hop : {fft / 2, fft / 4}) {
            for (bool reassign : {false, true}) {
                auto cfg = job_cfg((g_dir / "eq.wav").string(),
                                   (g_dir / "eq.png").string(), fft, hop);
                cfg.reassigned = reassign;
                Spectral::SpectralDataset ds;
                CHECK(Spectral::analyze_dataset(cfg, ds).ok(), "streaming analyzes");
                Reference ref;
                CHECK(decode_reference((g_dir / "eq.wav").string(), ref),
                      "reference decodes");
                CHECK(frames_exact(ds, ref, fft, hop, "hann"), "streaming==reference");
                if (reassign) {
                    // Reassignment arrays populated for every frame.
                    bool ok = ds.frame_count() > 0;
                    for (int f = 0; ok && f < ds.frame_count(); ++f) {
                        const auto& fr = ds.frame(f);
                        ok = fr.reassigned_freqs.size() == fr.magnitudes.size() &&
                             fr.reassigned_times.size() == fr.magnitudes.size();
                    }
                    CHECK(ok, "reassigned arrays complete");
                }
            }
        }
    }
    // Chunk-size invariance through the full pipeline incl. reassignment:
    // the decoder itself must deliver identical samples at any chunking
    // (verified below), so any dataset difference would be a feed bug.
    {
        Reference r1, r373;
        CHECK(decode_reference((g_dir / "eq.wav").string(), r1), "ref default decodes");
        CHECK(decode_reference((g_dir / "eq.wav").string(), r373, 373),
              "ref 373 decodes");
        CHECK(r1.mono == r373.mono, "decoder chunk-invariant");
        auto cfg = job_cfg((g_dir / "eq.wav").string(), (g_dir / "eq.png").string(),
                           512, 128);
        cfg.reassigned = true;
        Spectral::SpectralDataset a, b;
        CHECK(Spectral::analyze_dataset(cfg, a).ok(), "default chunks ok");
        CHECK(Spectral::analyze_dataset(cfg, b, {}, nullptr, 373).ok(),
              "prime-size chunks ok");
        bool same = a.frame_count() == b.frame_count();
        for (int f = 0; same && f < a.frame_count(); ++f) {
            const auto& fa = a.frame(f);
            const auto& fb = b.frame(f);
            same = fa.magnitudes == fb.magnitudes && fa.phases == fb.phases &&
                   fa.power == fb.power && fa.timestamp == fb.timestamp &&
                   fa.rms == fb.rms && fa.reassigned_freqs == fb.reassigned_freqs &&
                   fa.reassigned_times == fb.reassigned_times;
        }
        CHECK(same, "chunk-size invariant incl. reassignment");
    }
}

// --- 3. stereo mixing + sample rates ----------------------------------------------
static void test_mix_and_rates() {
    std::printf("[mix_and_rates]\n");
    const int sr = 22050, n = sr * 5;
    std::vector<float> left(static_cast<size_t>(n)), right(static_cast<size_t>(n)),
        mixed(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        left[static_cast<size_t>(i)] =
            0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / sr);
        right[static_cast<size_t>(i)] =
            0.25f * std::sin(2.0f * 3.14159265f * 880.0f * i / sr);
        mixed[static_cast<size_t>(i)] =
            (left[static_cast<size_t>(i)] + right[static_cast<size_t>(i)]) / 2.0f;
    }
    write_stereo_wav(g_dir / "st.wav", sr, left, right);
    write_wav_samples(g_dir / "mo.wav", sr, mixed);
    Spectral::SpectralDataset ds_st, ds_mo;
    CHECK(Spectral::analyze_dataset(
                  job_cfg((g_dir / "st.wav").string(), (g_dir / "x.png").string(), 512,
                          128),
                  ds_st)
              .ok(),
          "stereo analyzes");
    CHECK(Spectral::analyze_dataset(
                  job_cfg((g_dir / "mo.wav").string(), (g_dir / "x.png").string(), 512,
                          128),
                  ds_mo)
              .ok(),
          "mixed-mono analyzes");
    bool same = ds_st.frame_count() == ds_mo.frame_count();
    float worst = 0.0f;
    for (int f = 0; same && f < ds_st.frame_count(); ++f) {
        const auto& a = ds_st.frame(f).magnitudes;
        const auto& b = ds_mo.frame(f).magnitudes;
        if (a.size() != b.size()) {
            same = false;
            break;
        }
        for (size_t k = 0; k < a.size(); ++k) {
            const float d = std::fabs(a[k] - b[k]);
            if (d > worst) worst = d;
            // Fixture artifact, not mixing error: the stereo file quantizes
            // L/R separately while the mono file quantizes (L+R)/2, so the
            // two decodes differ by ~1 int16 LSB per sample. The bound below
            // (≈30 LSB accumulated coherently) is far below signal level.
            if (d > 1e-3f) {
                same = false;
                break;
            }
        }
    }
    CHECK(same, "stereo mixes exactly to (L+R)/2");
    CHECK(ds_st.channel_info().channels_mixed, "stereo marked mixed");
    CHECK(!ds_mo.channel_info().channels_mixed, "mono not marked mixed");
    // Sample rates: 8kHz fixture matches its own reference.
    write_wav_samples(g_dir / "s8k.wav", 8000, sine_mix(8000 * 5, 8000));
    Spectral::SpectralDataset ds8;
    CHECK(Spectral::analyze_dataset(
                  job_cfg((g_dir / "s8k.wav").string(), (g_dir / "x.png").string(), 256,
                          64),
                  ds8)
              .ok(),
          "8k analyzes");
    Reference ref;
    CHECK(decode_reference((g_dir / "s8k.wav").string(), ref), "8k reference decodes");
    CHECK(ref.sr == 8000, "native rate preserved");
    CHECK(frames_exact(ds8, ref, 256, 64, "hann"), "8k streaming==reference");
}

// --- 4. frame-count edges ------------------------------------------------------------
static void test_frame_edges() {
    std::printf("[frame_edges]\n");
    const int sr = 22050, fft = 512, hop = 256;
    auto analyze_n = [&](int samples, const char* name, Spectral::SpectralDataset& ds) {
        std::vector<float> v(static_cast<size_t>(std::max(0, samples)), 0.1f);
        const fs::path p = g_dir / (std::string("e") + name + ".wav");
        write_wav_samples(p, sr, v);
        return Spectral::analyze_dataset(
            job_cfg(p.string(), (g_dir / "x.png").string(), fft, hop), ds);
    };
    {
        Spectral::SpectralDataset ds;
        CHECK(analyze_n(fft, "512", ds).ok() && ds.frame_count() == 1,
              "exact-fft -> 1 frame");
    }
    {
        Spectral::SpectralDataset ds;
        CHECK(analyze_n(fft + 1, "513", ds).ok() && ds.frame_count() == 1,
              "fft+1 -> 1 frame");
    }
    {
        Spectral::SpectralDataset ds;
        CHECK(analyze_n(fft + hop - 1, "767", ds).ok() && ds.frame_count() == 1,
              "fft+H-1 -> 1 frame");
    }
    {
        Spectral::SpectralDataset ds;
        CHECK(analyze_n(fft + hop, "768", ds).ok() && ds.frame_count() == 2,
              "fft+H -> 2 frames");
    }
    {
        Spectral::SpectralDataset ds;
        CHECK(analyze_n(fft - 1, "511", ds) == Spectral::JobError::AnalysisError,
              "shorter-than-fft -> AnalysisError");
    }
    {
        Spectral::SpectralDataset ds;
        CHECK(analyze_n(0, "0", ds) == Spectral::JobError::DecodeError,
              "empty -> DecodeError");
    }
}

// --- 5. memory boundedness ---------------------------------------------------------------
static void test_memory_bounded() {
    std::printf("[memory_bounded]\n");
    const int sr = 22050;
    write_wav_samples(g_dir / "big.wav", sr, sine_mix(sr * 60, sr));
    Spectral::SpectralDataset ds;
    Spectral::AnalyzeStats stats;
    CHECK(Spectral::analyze_dataset(
                  job_cfg((g_dir / "big.wav").string(), (g_dir / "x.png").string(), 1024,
                          256),
                  ds, {}, nullptr, 0, &stats)
              .ok(),
          "60s analyzes");
    const int64_t expect = static_cast<int64_t>(sr) * 60;
    CHECK(stats.decoded_samples == expect, "decoded count exact");
    CHECK(stats.peak_buffered_samples <= static_cast<size_t>(2 * 1024 + 65536),
          "peak bounded by 2*fft+chunk");
    CHECK(stats.peak_buffered_samples * 4 < static_cast<size_t>(expect),
          "peak independent of duration");
    CHECK(ds.frame_count() == Spectral::stft_frame_count(static_cast<int>(expect),
                                                         1024, 256),
          "long-file frame count exact");
}

// --- 6. dataset invariants + video completeness ----------------------------------------------
static void test_invariants_and_video() {
    std::printf("[invariants_and_video]\n");
    Spectral::SpectralDataset ds;
    CHECK(Spectral::analyze_dataset(
                  job_cfg((g_dir / "eq.wav").string(), (g_dir / "x.png").string(), 512,
                          128),
                  ds)
              .ok(),
          "analyzes");
    bool contiguous = ds.frame_count() > 0;
    for (int f = 0; contiguous && f < ds.frame_count(); ++f) {
        const auto& fr = ds.frame(f);
        contiguous = fr.frame_index == f && fr.n_fft == 512 &&
                     fr.magnitudes.size() == 257 && fr.phases.size() == 257 &&
                     fr.power.size() == 257;
    }
    CHECK(contiguous, "indices contiguous, bins exact");
    CHECK(ds.num_frequency_bins() == 257, "bin count");
    CHECK(ds.analysis_metadata().analyzer_version == "2", "analyzer version");
    // Video still receives the complete spectral dataset.
    auto vcfg = job_cfg((g_dir / "eq.wav").string(), (g_dir / "v6.mp4").string(), 512,
                        128);
    vcfg.output_format = "video";
    vcfg.output_format_explicit = true;
    vcfg.width = 128;
    vcfg.height = 64;
    vcfg.fps = 10;
    fs::remove(g_dir / "v6.mp4");
    CHECK(Spectral::run_job(vcfg) == Spectral::JobError::Ok, "video job ok");
    CHECK(fs::exists(g_dir / "v6.mp4"), "video produced from streamed dataset");
}

// --- 7. cancellation still aborts the streaming loop ---------------------------------------------
static void test_streaming_cancel() {
    std::printf("[streaming_cancel]\n");
    const fs::path out = g_dir / "scancel.png";
    fs::remove(out);
    std::atomic<bool> flag{false};
    Spectral::Error result = Spectral::Error::success();
    std::thread worker([&] {
        auto cfg = job_cfg((g_dir / "big.wav").string(), out.string(), 1024, 256);
        cfg.reassigned = true;
        result = Spectral::run_job(
            cfg,
            [&](float f, const char* stage) {
                if (std::string(stage) == "analyze" && f > 0.05) flag.store(true);
            },
            &flag);
    });
    worker.join();
    CHECK(result == Spectral::JobError::Cancelled, "streaming cancel -> Cancelled");
    CHECK(!fs::exists(out), "no partial output after streaming cancel");
}

int main() {
    std::error_code ec;
    g_dir = fs::temp_directory_path(ec) / "svg_phase6_test";
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    if (ec) {
        std::printf("FAIL: cannot create test dir\n");
        return 1;
    }
    test_buffer_chunking();
    test_equivalence();
    test_mix_and_rates();
    test_frame_edges();
    test_memory_bounded();
    test_invariants_and_video();
    test_streaming_cancel();
    fs::remove_all(g_dir, ec);
    std::printf("\n=== streaming: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
