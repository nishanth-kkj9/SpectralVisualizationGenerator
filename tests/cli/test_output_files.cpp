// tests/cli/test_output_files.cpp
// Phase 4 — production-safe output handling: format/extension contract,
// collision-safe temp naming, safe replacement (old output survives a
// failed replace), temp lifecycle, batch output correctness, path behavior.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "batch.h"
#include "output_files.h"
#include "pipeline.h"
#include "video_encoder.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
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

static Spectral::GenerateConfig base_cfg(const std::string& in, const std::string& out) {
    Spectral::GenerateConfig cfg;
    cfg.input_path = in;
    cfg.output_path = out;
    cfg.fft_size = 512;
    cfg.width = 256;
    cfg.height = 128;
    return cfg;
}

static void write_wav(const fs::path& p, double freq_hz = 440.0) {
    const int sr = 22050, n = sr;  // 1s mono PCM16
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
        const auto s = static_cast<int16_t>(
            10000.0 * std::sin(2.0 * 3.14159265 * freq_hz * i / sr));
        f.write(reinterpret_cast<const char*>(&s), 2);
    }
}

static bool read_bytes(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

static bool has_temp_residue(const fs::path& dir) {
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (Spectral::is_temp_name(it->path().filename().string())) return true;
    }
    return false;
}

// --- 1-6. format/extension contract ------------------------------------
static void test_format_contract() {
    std::printf("[format_contract]\n");
    auto v = [](const std::string& out, const std::string& fmt, bool expl) {
        auto cfg = base_cfg("in.wav", out);
        cfg.output_format = fmt;
        cfg.output_format_explicit = expl;
        return Spectral::validate_config(cfg);
    };
    CHECK(v("a.png", "image", true).empty(), "image+.png valid");
    CHECK(v("a.PNG", "image", true).empty(), "image+.PNG valid (case)");
    CHECK(v("a", "image", true).empty(), "image extensionless valid");
    CHECK(!v("a.mp4", "image", true).empty(), "explicit image+.mp4 BadConfig");
    CHECK(!v("a.jpg", "image", true).empty(), "explicit image+.jpg BadConfig");
    CHECK(v("a.mp4", "video", true).empty(), "video+.mp4 valid");
    CHECK(v("a.webm", "video", true).empty(), "video+.webm valid");
    CHECK(v("a.mkv", "video", true).empty(), "video+.mkv valid");
    CHECK(v("a.MP4", "video", true).empty(), "video+.MP4 valid (case)");
    CHECK(!v("a.png", "video", true).empty(), "explicit video+.png BadConfig");
    CHECK(!v("a", "video", true).empty(), "explicit video extensionless BadConfig");
    CHECK(!v("a.avi", "video", true).empty(), "explicit video+.avi BadConfig");
    CHECK(v("a.mp4", "image", false).empty(), "inferred .mp4 valid");
    CHECK(Spectral::effective_output_format("a.mp4", "image", false) == "video",
          "inferred .mp4 resolves to video");
    CHECK(Spectral::effective_output_format("a.png", "image", false) == "image",
          "inferred .png resolves to image");
    CHECK(Spectral::effective_output_format("a.mp4", "image", true) == "image",
          "explicit format always wins resolution");
    CHECK(!v("a.jpg", "image", false).empty(), "inferred .jpg BadConfig");
    CHECK(!v(g_dir.string(), "image", false).empty(), "output-is-directory BadConfig");
}

// --- temp naming ---------------------------------------------------------
static void test_temp_naming() {
    std::printf("[temp_naming]\n");
    const std::string dst = (g_dir / "t.png").string();
    const std::string t1 = Spectral::make_temp_path(dst);
    const std::string t2 = Spectral::make_temp_path(dst);
    CHECK(t1 != t2, "two temps distinct");
    CHECK(fs::path(t1).parent_path() == g_dir, "temp in destination dir");
    CHECK(fs::path(t1).extension() == ".png", "temp keeps real extension");
    CHECK(Spectral::is_temp_name(fs::path(t1).filename().string()), "temp recognizable");
    // Existence-bump: occupying the first candidate moves the counter on.
    const std::string t3 = Spectral::make_temp_path(dst);
    { std::ofstream f(t3, std::ios::binary); f << "x"; }
    const std::string t4 = Spectral::make_temp_path(dst);
    CHECK(t3 != t4, "occupied temp name skipped");
    fs::remove(t3);
    CHECK(!Spectral::is_temp_name("t.png"), "final name not temp");
    CHECK(!Spectral::is_temp_name("partial.png"), "partial.png not temp");
    CHECK(Spectral::is_temp_name("t.part.png"), "legacy .part name recognized");
    // Concurrency: 8 threads x 50 temps, all distinct.
    std::vector<std::thread> th;
    std::vector<std::vector<std::string>> per(8);
    for (int k = 0; k < 8; ++k)
        th.emplace_back([&, k] {
            for (int i = 0; i < 50; ++i) per[k].push_back(Spectral::make_temp_path(dst));
        });
    for (auto& t : th) t.join();
    std::set<std::string> all;
    for (auto& v : per)
        for (auto& s : v) all.insert(s);
    CHECK(all.size() == 400, "400 concurrent temps all distinct");
}

// --- replacement ----------------------------------------------------------
static void test_commit() {
    std::printf("[commit]\n");
    const fs::path dst = g_dir / "c.png";
    const fs::path tmp = g_dir / "c.tmp";
    auto write = [](const fs::path& p, const char* s) {
        std::ofstream f(p, std::ios::binary);
        f << s;
    };
    // 16. no-destination replacement succeeds
    write(tmp, "NEW");
    CHECK(Spectral::commit_output(tmp.string(), dst.string()).empty(), "commit new ok");
    CHECK(!fs::exists(tmp), "temp gone after commit");
    std::vector<uint8_t> b;
    CHECK(read_bytes(dst, b) && b.size() == 3 && b[0] == 'N', "dst holds new bytes");
    // 13-14. existing output replaced with complete new file
    write(tmp, "NEWER!");
    CHECK(Spectral::commit_output(tmp.string(), dst.string()).empty(), "replace ok");
    CHECK(read_bytes(dst, b) && b.size() == 6 && b[0] == 'N', "dst holds replaced bytes");
    // 17. repeated replacement works
    for (int i = 0; i < 3; ++i) {
        write(tmp, "XYZ");
        CHECK(Spectral::commit_output(tmp.string(), dst.string()).empty(), "repeat replace ok");
    }
    CHECK(read_bytes(dst, b) && b.size() == 3 && b[0] == 'X', "repeat replace bytes");
    // Missing temp is an error, dst untouched.
    CHECK(!Spectral::commit_output((g_dir / "nope.tmp").string(), dst.string()).empty(),
          "missing temp errors");
    CHECK(read_bytes(dst, b) && b.size() == 3, "dst untouched on missing temp");
    // 15. forced replacement failure (exclusive lock = sharing violation):
    // old file must remain present and unchanged, temp cleaned up.
    write(dst, "OLD!");
    write(tmp, "NEW!!");
    HANDLE lock = CreateFileA(dst.string().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE, "exclusive lock acquired");
    const std::string err = Spectral::commit_output(tmp.string(), dst.string());
    CHECK(!err.empty(), "locked replace fails");
    CHECK(!fs::exists(tmp), "temp cleaned after failed replace");
    // The lock denies all sharing, so the bytes are verified after release.
    // A failed replace never touches the destination.
    CloseHandle(lock);
    CHECK(read_bytes(dst, b) && b.size() == 4 && b[0] == 'O' && b[3] == '!',
          "old file preserved on replace failure");
}

// --- run_job integration ----------------------------------------------------
static void test_job_output_safety() {
    std::printf("[job_output_safety]\n");
    write_wav(g_dir / "tone.wav");
    // 7. success produces final file only, no temp residue.
    {
        auto cfg = base_cfg((g_dir / "tone.wav").string(), (g_dir / "ok.png").string());
        CHECK(Spectral::run_job(cfg) == Spectral::JobError::Ok, "image job ok");
        CHECK(fs::exists(g_dir / "ok.png"), "final image exists");
        CHECK(!has_temp_residue(g_dir), "no temp residue after success");
    }
    // 9/11. failure before render keeps the old output, no temps.
    {
        const fs::path out = g_dir / "keep.png";
        auto good = base_cfg((g_dir / "tone.wav").string(), out.string());
        CHECK(Spectral::run_job(good) == Spectral::JobError::Ok, "baseline ok");
        std::vector<uint8_t> before;
        CHECK(read_bytes(out, before), "baseline readable");
        auto bad = base_cfg((g_dir / "missing.wav").string(), out.string());
        CHECK(Spectral::run_job(bad) != Spectral::JobError::Ok, "bad input fails");
        std::vector<uint8_t> after;
        CHECK(read_bytes(out, after) && after == before, "old output preserved on failure");
        CHECK(!has_temp_residue(g_dir), "no temp residue after failure");
    }
    // 13. existing output replaced by a new complete run.
    {
        const fs::path out = g_dir / "repl.png";
        auto cfg = base_cfg((g_dir / "tone.wav").string(), out.string());
        CHECK(Spectral::run_job(cfg) == Spectral::JobError::Ok, "first run ok");
        std::vector<uint8_t> a;
        CHECK(read_bytes(out, a), "first readable");
        CHECK(Spectral::run_job(cfg) == Spectral::JobError::Ok, "second run ok");
        std::vector<uint8_t> b2;
        CHECK(read_bytes(out, b2) && b2 == a, "replace deterministic");
        CHECK(!has_temp_residue(g_dir), "no temp residue after replace");
    }
    // 21-22. spaces + Unicode paths.
    {
        fs::create_directories(g_dir / "sp ace");
        write_wav(g_dir / "sp ace" / "tune.wav");
        auto cfg = base_cfg((g_dir / "sp ace" / "tune.wav").string(),
                            (g_dir / "sp ace" / "out file.png").string());
        CHECK(Spectral::run_job(cfg) == Spectral::JobError::Ok, "spaces path ok");
        auto cfg2 = base_cfg((g_dir / "tone.wav").string(),
                             (g_dir / "t\xC3\xA4st \xC3\xBCn\xC3\xAF.png").string());
        CHECK(Spectral::run_job(cfg2) == Spectral::JobError::Ok, "unicode path ok");
    }
    // 8. video success produces final only (needs ffmpeg; skip otherwise).
    if (Spectral::VideoEncoder::ffmpeg_available()) {
        auto cfg = base_cfg((g_dir / "tone.wav").string(), (g_dir / "ok.mp4").string());
        cfg.output_format = "video";
        cfg.output_format_explicit = true;
        cfg.width = 128;
        cfg.height = 64;
        cfg.fps = 10;
        CHECK(Spectral::run_job(cfg) == Spectral::JobError::Ok, "video job ok");
        CHECK(fs::exists(g_dir / "ok.mp4"), "final video exists");
        CHECK(!has_temp_residue(g_dir), "no temp residue after video");
    } else {
        std::printf("  (ffmpeg missing: video success test skipped)\n");
    }
    // 10. encode failure keeps the old output (bad codec, real ffmpeg gate).
    if (Spectral::VideoEncoder::ffmpeg_available()) {
        const fs::path out = g_dir / "keep.mp4";
        auto good = base_cfg((g_dir / "tone.wav").string(), out.string());
        good.output_format = "video";
        good.output_format_explicit = true;
        good.width = 128;
        good.height = 64;
        good.fps = 10;
        CHECK(Spectral::run_job(good) == Spectral::JobError::Ok, "video baseline ok");
        std::vector<uint8_t> before;
        CHECK(read_bytes(out, before), "video baseline readable");
        auto bad = good;
        bad.video_codec = "no_such_codec_xyz";
        CHECK(Spectral::run_job(bad) == Spectral::JobError::EncodeError,
              "bad codec -> EncodeError");
        std::vector<uint8_t> after;
        CHECK(read_bytes(out, after) && after == before, "old video preserved on encode failure");
        CHECK(!has_temp_residue(g_dir), "no temp residue after encode failure");
    }
}

// --- batch --------------------------------------------------------------------
static void test_batch_output() {
    std::printf("[batch_output]\n");
    const fs::path in = g_dir / "bin";
    const fs::path out = g_dir / "bout";
    fs::create_directories(in);
    write_wav(in / "song.wav");
    write_wav(in / "song.mp3");  // same stem: same planned output
    Spectral::GenerateConfig tpl;
    tpl.width = 128;
    tpl.height = 64;
    tpl.fft_size = 512;
    const std::atomic<bool> cancel{false};
    Spectral::BatchOptions opts;
    auto files = Spectral::collect_inputs(in.string(), opts);
    CHECK(files.size() == 2, "both stems collected");
    auto results = Spectral::run_batch(tpl, files, in.string(), out.string(), opts, cancel,
                                       {});
    CHECK(results.size() == 2, "two results");
    CHECK(!results[0].ok && !results[1].ok, "duplicate outputs both fail");
    CHECK(results[0].output == results[1].output, "duplicate outputs identical");
    CHECK(!results[0].error.empty(), "duplicate error message set");
    CHECK(!fs::exists(out / "song.png"), "no output for duplicates");
    // Temp files never scanned as inputs, even overlapping the input tree.
    {
        std::ofstream f(in / "ghost.512.7.part.mp4", std::ios::binary);
        f << "junk";
    }
    auto files2 = Spectral::collect_inputs(in.string(), opts);
    for (auto& p : files2)
        CHECK(p.find(".part.") == std::string::npos, "no temp file collected");
    // Unwritable output root fails with the directory cause.
    {
        const fs::path blocker = g_dir / "blocker";
        { std::ofstream f(blocker, std::ios::binary); f << "x"; }
        auto r = Spectral::run_batch(tpl, files, in.string(), blocker.string(), opts, cancel,
                                     {});
        CHECK(!r.empty() && !r[0].ok, "blocked output dir fails");
        CHECK(r[0].error.find("cannot create output directory") != std::string::npos,
              "directory cause reported");
    }
}

int main() {
    std::error_code ec;
    g_dir = fs::temp_directory_path(ec) / "svg_phase4_test";
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    if (ec) {
        std::printf("FAIL: cannot create test dir\n");
        return 1;
    }
    test_format_contract();
    test_temp_naming();
    test_commit();
    test_job_output_safety();
    test_batch_output();
    fs::remove_all(g_dir, ec);
    std::printf("\n=== output_files: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
