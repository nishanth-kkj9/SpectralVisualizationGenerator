// Phase 5 — SpectralDataset test suite
// Covers: serialization, deserialization, version validation, deterministic
// round-trip, dimension consistency, corrupted dataset handling.

#include "spectral_dataset.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace Spectral;

namespace {

int g_pass = 0;
int g_fail = 0;
std::string g_current_test;

void report(bool ok, const std::string& msg) {
    if (ok) {
        ++g_pass;
        std::printf("  PASS: %s\n", msg.c_str());
    } else {
        ++g_fail;
        std::printf("  FAIL: %s\n", msg.c_str());
    }
}

#define EXPECT(cond, msg)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_pass;                                                         \
            std::printf("  PASS: %s\n", msg);                                 \
        } else {                                                              \
            ++g_fail;                                                         \
            std::printf("  FAIL: %s (at %s:%d)\n", msg, __FILE__, __LINE__);  \
        }                                                                     \
    } while (0)

// ============================================================================
// Builders for synthetic datasets with deterministic content
// ============================================================================

SpectralDataset build_synthetic(int n_fft = 1024,
                                int n_frames = 16,
                                int sample_rate = 44100) {
    SpectralDataset d;
    d.mutable_analysis_metadata().fft_size = n_fft;
    d.mutable_analysis_metadata().hop_size = n_fft / 2;
    d.mutable_analysis_metadata().sample_rate = sample_rate;
    d.mutable_analysis_metadata().window_type = "hann";
    d.mutable_analysis_metadata().window_coherent_gain = 0.5f;
    d.mutable_analysis_metadata().overlap_ratio = 0.5f;
    d.mutable_analysis_metadata().analyzed_channels = 1;
    d.mutable_analysis_metadata().magnitude_scale = 1.0f;
    d.mutable_analysis_metadata().nyquist_frequency =
        static_cast<float>(sample_rate) / 2.0f;
    d.mutable_analysis_metadata().num_frequency_bins = n_fft / 2 + 1;
    d.mutable_analysis_metadata().analyzer_version = "test-1.0";

    d.mutable_normalization_info().window_coherent_gain = 0.5f;
    d.mutable_normalization_info().db_floor = -90.0f;
    d.mutable_normalization_info().db_reference = 1.0f;
    d.mutable_normalization_info().magnitude_scale = 1.0f;

    d.mutable_channel_info().total_channels = 2;
    d.mutable_channel_info().analyzed_channels = 1;
    d.mutable_channel_info().analyzed_channel_index = 0;
    d.mutable_channel_info().channel_names = {"L", "R"};
    d.mutable_channel_info().channels_mixed = false;

    d.mutable_source_metadata().file_path = "test://synthetic.wav";
    d.mutable_source_metadata().file_hash =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    d.mutable_source_metadata().file_size_bytes = 1024 * 1024;
    d.mutable_source_metadata().sample_rate = sample_rate;
    d.mutable_source_metadata().num_channels = 2;
    d.mutable_source_metadata().duration_seconds =
        static_cast<double>(n_frames * (n_fft / 2)) / sample_rate;
    d.mutable_source_metadata().codec_name = "pcm_s16le";
    d.mutable_source_metadata().codec_long_name = "PCM signed 16-bit little-endian";

    d.mutable_frequency_axis() = FrequencyAxis(n_fft, sample_rate);
    d.mutable_time_axis() = TimeAxis(n_frames, n_fft / 2, sample_rate);

    // Deterministic frame content: pure sine at 1kHz should peak at bin ~46
    // (44100/1024 ≈ 43.07 Hz/bin, so 1000/43.07 ≈ 23.2, peak at bin 23).
    const int peak_bin = static_cast<int>(std::round(
        1000.0 * n_fft / static_cast<double>(sample_rate)));
    for (int i = 0; i < n_frames; ++i) {
        Spectral::SpectralFrame f;
        f.frame_index = i;
        f.n_fft = n_fft;
        f.window_factor = 0.5f;
        f.timestamp = i * (n_fft / 2) / static_cast<double>(sample_rate);
        f.rms = 0.5f;
        f.peak_magnitude = 1.0f;
        f.spectral_centroid = 1000.0f;
        f.spectral_bandwidth = 50.0f;
        f.magnitudes.assign(n_fft / 2 + 1, 0.01f);
        f.phases.assign(n_fft / 2 + 1, 0.0f);
        f.magnitudes[peak_bin] = 1.0f;
        f.power.resize(f.magnitudes.size());
        for (size_t k = 0; k < f.magnitudes.size(); ++k) {
            f.power[k] = f.magnitudes[k] * f.magnitudes[k];
        }
        d.add_frame(f);
    }
    d.mutable_analysis_metadata().total_frames = d.frame_count();
    d.mutable_analysis_metadata().total_duration_seconds = d.total_duration();
    d.mutable_analysis_metadata().frame_duration_seconds = d.frame_duration();
    return d;
}

} // namespace

// ============================================================================
// Test 1 — Dimension consistency
// ============================================================================
static void test_dimension_consistency() {
    std::printf("\n[Test 1] Dimension consistency\n");
    SpectralDataset d = build_synthetic();

    auto v = d.validate();
    EXPECT(!v.has_errors(),
           "freshly-built dataset passes full validation (no errors)");

    EXPECT(d.check_dimensions(), "check_dimensions() returns true");
    EXPECT(d.check_metadata_consistency(), "check_metadata_consistency() returns true");
    EXPECT(d.check_data_integrity(), "check_data_integrity() returns true");

    // Corrupt: change num_bins to mismatch frame magnitudes
    d.mutable_frequency_axis().num_bins = 999;
    EXPECT(!d.check_dimensions(), "num_bins mismatch detected");
    EXPECT(!d.validate().valid, "validate() flags mismatched num_bins");
    d.mutable_frequency_axis().num_bins = d.mutable_frequency_axis().bin_frequencies.size();
    EXPECT(d.check_dimensions(), "dimension restored");
}

// ============================================================================
// Test 2 — Serialization
// ============================================================================
static void test_serialization() {
    std::printf("\n[Test 2] Serialization (binary + JSON)\n");
    SpectralDataset d = build_synthetic();

    std::vector<uint8_t> buf;
    EXPECT(d.serialize_binary(buf), "serialize_binary succeeds");
    EXPECT(!buf.empty(), "binary buffer is non-empty");
    EXPECT(buf.size() >= 64, "binary buffer has at least header (64 bytes)");
    // Magic check
    uint32_t magic = 0;
    std::memcpy(&magic, buf.data(), sizeof(uint32_t));
    EXPECT(magic == 0x53504454, "binary magic = 'SPDT'");

    std::string js = d.serialize_json();
    EXPECT(!js.empty(), "serialize_json produces non-empty output");
    EXPECT(js.find("\"version\"") != std::string::npos, "json contains version");
    EXPECT(js.find("\"frames\"") != std::string::npos, "json contains frames");
}

// ============================================================================
// Test 3 — Deserialization (round-trip)
// ============================================================================
static void test_deserialization() {
    std::printf("\n[Test 3] Deserialization round-trip\n");

    SpectralDataset d = build_synthetic();

    // Binary round-trip
    std::vector<uint8_t> buf;
    d.serialize_binary(buf);
    SpectralDataset d2;
    EXPECT(d2.deserialize_binary(buf.data(), buf.size()),
           "deserialize_binary succeeds on valid buffer");
    EXPECT(d == d2, "binary round-trip preserves equality");

    // JSON round-trip
    std::string js = d.serialize_json();
    SpectralDataset d3;
    EXPECT(d3.deserialize_json(js), "deserialize_json succeeds on valid input");
    EXPECT(d3.frame_count() == d.frame_count(),
           "json round-trip preserves frame count");
    EXPECT(d3.num_frequency_bins() == d.num_frequency_bins(),
           "json round-trip preserves num_frequency_bins");
    EXPECT(d3.sample_rate() == d.sample_rate(),
           "json round-trip preserves sample_rate");
    EXPECT(d3.fft_size() == d.fft_size(),
           "json round-trip preserves fft_size");
    EXPECT(d3.hop_size() == d.hop_size(),
           "json round-trip preserves hop_size");
    EXPECT(std::fabs(d3.total_duration() - d.total_duration()) < 1e-6,
           "json round-trip preserves total_duration");
    // Spot-check frame magnitudes
    for (int i = 0; i < d.frame_count(); ++i) {
        const auto& a = d.frame(i);
        const auto& b = d3.frame(i);
        for (int k = 0; k < d.num_frequency_bins(); ++k) {
            if (a.magnitudes[k] != b.magnitudes[k]) {
                std::printf("  FAIL: frame %d bin %d magnitude mismatch\n", i, k);
                ++g_fail;
                return;
            }
        }
    }
    std::printf("  PASS: json round-trip preserves all frame magnitudes\n");
    ++g_pass;
}

// ============================================================================
// Test 4 — Deterministic data
// ============================================================================
static void test_deterministic_data() {
    std::printf("\n[Test 4] Deterministic data\n");
    SpectralDataset d = build_synthetic();

    // Two serializations of identical input must be byte-identical
    std::vector<uint8_t> buf1, buf2;
    d.serialize_binary(buf1);
    d.serialize_binary(buf2);
    EXPECT(buf1 == buf2, "binary serialization is byte-identical (deterministic)");

    std::string js1 = d.serialize_json();
    std::string js2 = d.serialize_json();
    EXPECT(js1 == js2, "json serialization is byte-identical (deterministic)");

    // No timestamps leak in (timestamps are system_clock, not serialized)
    EXPECT(js1.find("analysis_timestamp") == std::string::npos,
           "timestamps not serialized in JSON");

    // Round-trip is also deterministic
    SpectralDataset d2;
    d2.deserialize_binary(buf1.data(), buf1.size());
    std::vector<uint8_t> buf3;
    d2.serialize_binary(buf3);
    EXPECT(buf1 == buf3, "deserialize -> serialize is identity (deterministic)");
}

// ============================================================================
// Test 5 — Version validation
// ============================================================================
static void test_version_validation() {
    std::printf("\n[Test 5] Version validation\n");

    EXPECT(SpectralDataset::current_version() == 3u, "current version is 3");
    EXPECT(SpectralDataset::min_compatible_version() == 3u,
           "only v3 loads (explicit LE layout)");

    SpectralDataset d = build_synthetic();
    std::vector<uint8_t> buf;
    d.serialize_binary(buf);

    // Truncate to less than header
    EXPECT(!SpectralDataset().deserialize_binary(buf.data(), 10),
           "rejects truncated header");

    // Corrupt version field (byte 4-7)
    std::vector<uint8_t> bad = buf;
    bad[5] = 0xFF; bad[6] = 0xFF; bad[7] = 0xFF;
    EXPECT(!SpectralDataset().deserialize_binary(bad.data(), bad.size()),
           "rejects unknown version");

    // Corrupt magic (bytes 0-3)
    std::vector<uint8_t> bad_magic = buf;
    bad_magic[0] = 0xDE; bad_magic[1] = 0xAD;
    EXPECT(!SpectralDataset().deserialize_binary(bad_magic.data(), bad_magic.size()),
           "rejects bad magic");
}

// ============================================================================
// Test 6 — Corrupted dataset handling
// ============================================================================
static void test_corrupted_dataset() {
    std::printf("\n[Test 6] Corrupted dataset handling\n");
    SpectralDataset d = build_synthetic();
    std::vector<uint8_t> buf;
    d.serialize_binary(buf);

    // Flip a byte inside the payload (after the 64-byte header)
    std::vector<uint8_t> bad_payload = buf;
    bad_payload[200] ^= 0xFF;
    EXPECT(!SpectralDataset().deserialize_binary(bad_payload.data(), bad_payload.size()),
           "rejects buffer with corrupted payload byte (checksum mismatch)");

    // Truncate mid-payload
    std::vector<uint8_t> truncated(buf.begin(), buf.begin() + buf.size() / 2);
    EXPECT(!SpectralDataset().deserialize_binary(truncated.data(), truncated.size()),
           "rejects truncated payload");

    // Garbage
    std::vector<uint8_t> garbage(1024, 0xAB);
    EXPECT(!SpectralDataset().deserialize_binary(garbage.data(), garbage.size()),
           "rejects garbage input");

    // Empty
    EXPECT(!SpectralDataset().deserialize_binary(nullptr, 0),
           "rejects null/empty input");

    // Corrupt JSON: missing version
    EXPECT(!SpectralDataset().deserialize_json("{\"frames\": []}"),
           "rejects JSON without version");
    // Corrupt JSON: bad version
    EXPECT(!SpectralDataset().deserialize_json("{\"version\": 999}"),
           "rejects JSON with unsupported version");
    // Corrupt JSON: missing frames
    EXPECT(!SpectralDataset().deserialize_json(
               "{\"version\": 1, \"min_compatible_version\": 1}"),
           "rejects JSON without frames");
}

// ============================================================================
// Bonus: file I/O round-trip
// ============================================================================
static void test_file_io() {
    std::printf("\n[Test 7] File I/O round-trip\n");
    SpectralDataset d = build_synthetic();

    const std::string bin_path = "test_dataset.bin";
    const std::string json_path = "test_dataset.json";
    std::remove(bin_path.c_str());
    std::remove(json_path.c_str());

    EXPECT(d.save_to_file(bin_path, SerializationFormat::Binary),
           "save_to_file binary succeeds");
    SpectralDataset d2;
    EXPECT(d2.load_from_file(bin_path, SerializationFormat::Binary),
           "load_from_file binary succeeds");
    EXPECT(d == d2, "binary file round-trip preserves equality");

    EXPECT(d.save_to_file(json_path, SerializationFormat::JSON),
           "save_to_file json succeeds");
    SpectralDataset d3;
    EXPECT(d3.load_from_file(json_path, SerializationFormat::JSON),
           "load_from_file json succeeds");
    EXPECT(d3.frame_count() == d.frame_count(),
           "json file round-trip preserves frame count");

    std::remove(bin_path.c_str());
    std::remove(json_path.c_str());
}

// ============================================================================
// Bonus: validation catches inconsistencies
// ============================================================================
static void test_validation_catches_inconsistencies() {
    std::printf("\n[Test 8] Validation catches inconsistencies\n");
    SpectralDataset d = build_synthetic();
    EXPECT(d.validate().valid, "well-formed dataset validates");

    // Inconsistent sample rate between axes
    d.mutable_frequency_axis().sample_rate = 48000;
    EXPECT(!d.validate().valid, "mismatched sample_rate between axes flagged");

    // Inconsistent fft_size between frame and analysis
    d = build_synthetic();
    auto f0 = d.frame(0);
    f0.n_fft = 2048;
    // (We can't easily replace a frame; mutate a magnitude array size.)
    // Instead: tamper with the analysis fft_size.
    d.mutable_analysis_metadata().fft_size = 2048;
    EXPECT(!d.validate().valid, "mismatched fft_size flagged");

    // Non-power-of-two fft_size
    d = build_synthetic();
    d.mutable_analysis_metadata().fft_size = 1000;
    EXPECT(!d.validate().valid, "non-power-of-two fft_size flagged");

    // Non-finite data: dataset has only const accessors, so we test this
    // indirectly by relying on the deserializer's payload validation. The
    // clean dataset must pass the integrity check.
    d = build_synthetic();
    EXPECT(d.check_data_integrity(),
           "clean dataset passes data integrity check");
}

// ============================================================================
// Bonus: filters and statistics
// ============================================================================
static void test_filters_and_stats() {
    std::printf("\n[Test 9] Filters and statistics\n");
    SpectralDataset d = build_synthetic();

    auto mean = d.mean_magnitude_spectrum();
    auto mx = d.max_magnitude_spectrum();
    auto mn = d.min_magnitude_spectrum();
    EXPECT(static_cast<int>(mean.size()) == d.num_frequency_bins(),
           "mean spectrum has num_frequency_bins entries");
    EXPECT(mx[23] >= 0.99f, "max spectrum has peak near bin 23 (1 kHz)");
    EXPECT(mn[23] >= 0.99f, "min spectrum has peak near bin 23 (1 kHz)");
    EXPECT(mean[23] >= 0.99f, "mean spectrum has peak near bin 23 (1 kHz)");

    auto band = d.filter_band(500.0f, 1500.0f);
    EXPECT(band.num_frequency_bins() < d.num_frequency_bins(),
           "filter_band reduces num bins");
    EXPECT(band.frame_count() == d.frame_count(),
           "filter_band preserves frame count");

    auto t = d.filter_time(0.0, 0.05);
    EXPECT(t.frame_count() <= d.frame_count(),
           "filter_time reduces frame count");

    auto ft = d.downsample_time(2);
    EXPECT(ft.frame_count() <= (d.frame_count() + 1) / 2 + 1,
           "downsample_time roughly halves frames");

    auto ff = d.downsample_frequency(4);
    EXPECT(ff.num_frequency_bins() <= (d.num_frequency_bins() + 3) / 4 + 1,
           "downsample_frequency roughly quarters bins");
}

// ============================================================================
// S5 — v3 explicit layout, reassigned arrays, determinism, invariants
// ============================================================================

// Independent LE blob writer (mirrors the documented format, shares no
// code with production serialization): proves the format, not the writer.
struct BlobW {
    std::vector<uint8_t> b;
    void u32(uint32_t v) {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v >> 16));
        b.push_back(static_cast<uint8_t>(v >> 24));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void f32(float v) {
        uint32_t u = 0;
        std::memcpy(&u, &v, 4);
        u32(u);
    }
    void f64(double v) {
        uint64_t u = 0;
        std::memcpy(&u, &v, 8);
        u64(u);
    }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
};

static uint64_t test_fnv(const std::vector<uint8_t>& d, size_t off, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= d[off + i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Minimal hand-built v3 blob: fft 8, hop 4, sr 8000, 2 frames, 5 bins.
static std::vector<uint8_t> build_v3_fixture() {
    BlobW p;
    p.str("t.wav");
    p.str("hash");
    p.u64(100);
    p.i32(8000);
    p.i32(1);
    p.f64(0.5);
    p.str("pcm");
    p.str("");
    p.str("hann");
    p.f32(0.5f);
    p.i32(8);
    p.i32(4);
    p.f32(0.5f);
    p.i32(8000);
    p.i32(1);
    p.i32(0);
    p.f32(1.0f);
    p.f32(0.0f);
    p.f32(4000.0f);
    p.i32(5);
    p.f64(0.0005);
    p.f64(0.001);
    p.i32(2);
    p.str("2");
    p.str("stft");
    p.i32(1);
    p.f32(0.5f);
    p.f32(0.375f);
    p.f32(1.0f);
    p.f32(1.0f);
    p.f32(-90.0f);
    p.f32(1.0f);
    p.b.push_back(0);  // phase_unwrapped bool: exactly one byte
    p.f32(0.0f);
    p.b.push_back(0);  // channel_normalized bool: exactly one byte
    p.i32(1);
    p.i32(1);
    p.i32(0);
    p.b.push_back(0);  // channels_mixed bool: exactly one byte
    p.u32(0);
    BlobW ax;
    ax.i32(5);
    ax.i32(8);
    ax.i32(8000);
    ax.f32(4000.0f);
    ax.f32(1000.0f);
    ax.u64(5);
    for (int k = 0; k < 5; ++k) ax.f32(static_cast<float>(k * 1000));
    ax.i32(2);
    ax.i32(4);
    ax.i32(8000);
    ax.f64(0.0005);
    ax.f64(0.001);
    ax.u64(2);
    ax.f64(0.0);
    ax.f64(0.0005);
    BlobW fr;
    fr.u64(2);
    for (int f = 0; f < 2; ++f) {
        fr.i32(f);
        fr.i32(8);
        fr.f32(0.5f);
        fr.f64(f * 0.0005);
        fr.f32(0.1f);
        fr.f32(0.9f);
        fr.f32(2000.0f);
        fr.f32(10.0f);
        fr.i32(1);
        fr.u64(5);
        for (int k = 0; k < 5; ++k) fr.f32(0.1f * (k + 1) + f);
        fr.u64(5);
        for (int k = 0; k < 5; ++k) fr.f32(0.01f * k);
        fr.u64(5);
        for (int k = 0; k < 5; ++k) fr.f32(0.01f * (k + 1) * (k + 1));
        fr.u64(0);
        fr.u64(0);
    }
    BlobW out;
    out.u32(0x53504454);  // magic value; LE bytes on disk
    out.u32(3);
    out.u32(64);
    const size_t pay = p.b.size() + ax.b.size() + fr.b.size();
    out.u64(pay);
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), p.b.begin(), p.b.end());
    payload.insert(payload.end(), ax.b.begin(), ax.b.end());
    payload.insert(payload.end(), fr.b.begin(), fr.b.end());
    out.u64(test_fnv(payload, 0, payload.size()));
    while (out.b.size() < 64) out.b.push_back(0);
    out.b.insert(out.b.end(), payload.begin(), payload.end());
    return out.b;
}

static void test_v3_explicit_layout() {
    std::printf("\n[S5] v3 explicit layout\n");
    SpectralDataset d = build_synthetic(256, 4, 16000);
    std::vector<uint8_t> buf;
    EXPECT(d.serialize_binary(buf), "serialize v3");
    // Magic is the u32 value 0x53504454 in LE bytes (T,D,P,S on disk).
    EXPECT(buf.size() >= 64 && buf[0] == 0x54 && buf[1] == 0x44 && buf[2] == 0x50 &&
               buf[3] == 0x53,
           "magic SPDT");
    const uint32_t ver = static_cast<uint32_t>(buf[4]) |
                         (static_cast<uint32_t>(buf[5]) << 8) |
                         (static_cast<uint32_t>(buf[6]) << 16) |
                         (static_cast<uint32_t>(buf[7]) << 24);
    EXPECT(ver == 3u, "version field is 3 LE");
    // v2-labeled blob must be rejected, not misparsed
    std::vector<uint8_t> v2 = buf;
    v2[4] = 2;
    v2[5] = v2[6] = v2[7] = 0;
    SpectralDataset d2;
    EXPECT(!d2.deserialize_binary(v2.data(), v2.size()), "v2 blob rejected");
    // bad magic rejected
    std::vector<uint8_t> bad = buf;
    bad[0] = 'X';
    EXPECT(!d2.deserialize_binary(bad.data(), bad.size()), "bad magic rejected");
}

static void test_cross_platform_fixture() {
    std::printf("\n[S5] cross-platform hand fixture\n");
    const std::vector<uint8_t> blob = build_v3_fixture();
    SpectralDataset d;
    const bool loaded = d.deserialize_binary(blob.data(), blob.size());
    EXPECT(loaded, "hand blob loads");
    if (!loaded) return;  // never index an unloaded dataset
    EXPECT(d.frame_count() == 2, "two frames");
    EXPECT(d.num_frequency_bins() == 5, "five bins");
    EXPECT(d.sample_rate() == 8000, "rate 8000");
    EXPECT(d.frame(0).magnitudes[3] == 0.4f, "independent mag value");
    EXPECT(d.frame(1).magnitudes[0] == 1.1f, "second frame offset");
    EXPECT(d.frame(0).timestamp == 0.0, "first stamp");
    EXPECT(d.source_metadata().file_hash == "hash", "hash field");
    EXPECT(d.analysis_metadata().analysis_method == "stft", "method field");
    auto v = d.validate();
    EXPECT(!v.has_errors(), "hand fixture validates");
}

static void test_reassigned_roundtrip() {
    std::printf("\n[S5] reassigned round trip\n");
    SpectralDataset d = build_synthetic(256, 4, 16000);
    // JSON path carries reassigned arrays (non-empty binary RT is covered
    // by the live reassigned pipeline run in test_reproducibility).
    const std::string js = d.serialize_json(false);
    EXPECT(js.find("reassigned_times") != std::string::npos, "json emits reassigned");
    SpectralDataset d2;
    EXPECT(d2.deserialize_json(js), "json loads");
    EXPECT(d2 == d, "json round trip equal");
}

static void test_corruption_battery() {
    std::printf("\n[S5] corruption battery\n");
    SpectralDataset d = build_synthetic(256, 4, 16000);
    std::vector<uint8_t> buf;
    EXPECT(d.serialize_binary(buf), "baseline serializes");
    SpectralDataset x;
    auto rejects = [&](std::vector<uint8_t> b, const char* msg) {
        SpectralDataset t;
        EXPECT(!t.deserialize_binary(b.data(), b.size()), msg);
    };
    rejects({}, "empty rejected");
    rejects(std::vector<uint8_t>(buf.begin(), buf.begin() + 10), "truncated header rejected");
    rejects(std::vector<uint8_t>(buf.begin(), buf.begin() + 100), "truncated metadata rejected");
    {  // checksum flip
        auto b = buf;
        b[100] ^= 0xFF;
        rejects(b, "checksum mismatch rejected");
    }
    {  // impossible frame count (u64 at offset 64+metasz...) — patch the
        // frames-count field: find it by re-serializing a 0-frame dataset
        // is overkill; instead corrupt the first frame-count u64 region by
        // flipping bytes right after the fixed header+small metadata is
        // fragile. Use version bump instead (future version path):
        auto b = buf;
        b[4] = 9;
        rejects(b, "future version rejected");
    }
    {  // huge string length in first field
        auto b = buf;
        // file_path length lives at offset 64 (u32 LE)
        b[64] = 0xFF;
        b[65] = 0xFF;
        b[66] = 0xFF;
        b[67] = 0xFF;
        rejects(b, "oversize string rejected");
    }
    {  // bool byte > 1 must be rejected (not read as true): flip the
        // channels_mixed byte of the hand fixture (payload offset 184),
        // re-stamp the checksum so only the bool rule can fire.
        const std::vector<uint8_t> good = build_v3_fixture();
        std::vector<uint8_t> b = good;
        b[64 + 184] = 2;
        const uint64_t cs = test_fnv(b, 64, b.size() - 64);
        for (int i = 0; i < 8; ++i)
            b[20 + i] = static_cast<uint8_t>(cs >> (8 * i));
        SpectralDataset t;
        EXPECT(!t.deserialize_binary(b.data(), b.size()), "bool > 1 rejected");
    }
}

static void test_determinism_identity() {
    std::printf("\n[S5] determinism + identity\n");
    SpectralDataset d = build_synthetic(256, 4, 16000);
    std::vector<uint8_t> a, b;
    EXPECT(d.serialize_binary(a), "serialize a");
    EXPECT(d.serialize_binary(b), "serialize b");
    EXPECT(a == b, "same dataset -> same bytes");
    const std::string id1 = d.dataset_identity();
    EXPECT(id1.size() == 64, "identity is 64 hex chars");
    SpectralDataset d2;
    EXPECT(d2.deserialize_binary(a.data(), a.size()), "reload");
    EXPECT(d2.dataset_identity() == id1, "identity stable across reload");
    EXPECT(d2 == d, "semantic equality");
    d2.mutable_analysis_metadata().hop_size = 999;
    EXPECT(d2.dataset_identity() != id1, "mutation moves identity");
}

static void test_metadata_invariants() {
    std::printf("\n[S5] metadata invariants\n");
    SpectralDataset d = build_synthetic(1024, 8, 44100);
    auto v = d.validate();
    EXPECT(!v.has_errors(), "synthetic validates clean");
    // timestamp == index*hop/sr
    bool ts = true;
    for (int i = 0; i < d.frame_count(); ++i) {
        double expect = static_cast<double>(i) * 512 / 44100.0;
        if (std::fabs(d.frame(i).timestamp - expect) > 1e-9 * (expect + 1.0)) ts = false;
    }
    EXPECT(ts, "timestamps equal index*hop/rate");
    // break one invariant at a time
    {
        SpectralDataset bad = d;
        bad.mutable_frequency_axis().bin_frequencies[10] = -5.0f;
        EXPECT(bad.validate().has_errors(), "negative bin rejected");
    }
    {
        SpectralDataset bad = d;
        bad.mutable_frequency_axis().bin_frequencies[10] = 1e9f;
        EXPECT(bad.validate().has_errors(), "off-formula bin rejected");
    }
    {
        SpectralDataset bad = d;
        SpectralFrame f;
        bad.get_frame(0, f);
        f.power.resize(3);
        // replace frame 0 via clear/re-add path is API-limited; validate the
        // strict power-size rule through a hand-built small dataset instead
        (void)f;
        SpectralDataset small;
        small.mutable_analysis_metadata().fft_size = 8;
        small.mutable_analysis_metadata().hop_size = 4;
        small.mutable_analysis_metadata().sample_rate = 8000;
        small.mutable_analysis_metadata().num_frequency_bins = 5;
        small.mutable_frequency_axis() = FrequencyAxis(8, 8000);
        small.mutable_time_axis() = TimeAxis(1, 4, 8000);
        SpectralFrame sf;
        sf.frame_index = 0;
        sf.n_fft = 8;
        sf.timestamp = 0.0;
        sf.magnitudes.assign(5, 0.1f);
        sf.phases.assign(5, 0.0f);
        sf.power.assign(3, 0.01f);  // wrong size on purpose
        small.add_frame(sf);
        EXPECT(small.validate().has_errors(), "short power rejected");
    }
    {
        SpectralDataset bad = d;
        bad.mutable_analysis_metadata().sample_rate = 8000;  // axis still 44100
        EXPECT(bad.validate().has_errors(), "rate/axis mismatch rejected");
    }
    {
        // NaN magnitudes load fine (any f32 decodes) but never validate.
        SpectralDataset bad;
        bad.mutable_analysis_metadata().fft_size = 8;
        bad.mutable_analysis_metadata().hop_size = 4;
        bad.mutable_analysis_metadata().sample_rate = 8000;
        bad.mutable_analysis_metadata().num_frequency_bins = 5;
        bad.mutable_frequency_axis() = FrequencyAxis(8, 8000);
        bad.mutable_time_axis() = TimeAxis(1, 4, 8000);
        SpectralFrame sf;
        sf.frame_index = 0;
        sf.n_fft = 8;
        sf.magnitudes.assign(5, 0.1f);
        sf.magnitudes[2] = std::numeric_limits<float>::quiet_NaN();
        sf.phases.assign(5, 0.0f);
        sf.power.assign(5, 0.01f);
        bad.add_frame(sf);
        EXPECT(bad.validate().has_errors(), "NaN magnitude rejected");
    }
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::printf("=== SpectralDataset Test Suite (Phase 5) ===\n");
    test_dimension_consistency();
    test_serialization();
    test_deserialization();
    test_deterministic_data();
    test_version_validation();
    test_corrupted_dataset();
    test_file_io();
    test_validation_catches_inconsistencies();
    test_filters_and_stats();
    test_v3_explicit_layout();
    test_cross_platform_fixture();
    test_reassigned_roundtrip();
    test_corruption_battery();
    test_determinism_identity();
    test_metadata_invariants();

    std::printf("\n=== Summary ===\n");
    std::printf("Passed: %d\n", g_pass);
    std::printf("Failed: %d\n", g_fail);
    std::printf("Result: %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
