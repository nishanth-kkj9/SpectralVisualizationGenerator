// tests/cli/test_cancellation.cpp
// Phase 5 — genuine end-to-end cancellation. Every test requests
// cancellation WHILE work is provably in progress (progress callbacks,
// byte counters, or output-file appearance drive the request — never a
// preset flag), then proves the work actually stopped: Cancelled code,
// no partial output, temp removed, old output intact, ffmpeg dead.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

#include "batch.h"
#include "media_decoder.h"
#include "output_files.h"
#include "pipeline.h"
#include "process/safe_process.h"
#include "video_encoder.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
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
static std::string g_spectragen;  // argv[1]: path to spectragen.exe (CLI test)

// Poll until fn() is true (5ms steps). Timeout fails honestly; success
// means the waited-for work state was really observed, not slept past.
static bool wait_until(const std::function<bool()>& fn, int timeout_ms = 30000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (fn()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return fn();
}

static void write_wav(const fs::path& p, int seconds, double freq_hz = 440.0) {
    const int sr = 22050, n = sr * seconds;
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

static int count_ffmpeg() {
    DWORD pids[1024];
    DWORD needed = 0;
    if (!EnumProcesses(pids, sizeof(pids), &needed)) return -1;
    int n = 0;
    const DWORD count = needed / sizeof(DWORD);
    for (DWORD i = 0; i < count; ++i) {
        HANDLE h =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
        if (!h) continue;
        char path[MAX_PATH] = {0};
        if (GetProcessImageFileNameA(h, path, MAX_PATH) > 0) {
            std::string s = path;
            for (auto& c : s) c = static_cast<char>(std::tolower(c));
            const size_t sep = s.find_last_of("\\/");
            const std::string base =
                (sep == std::string::npos) ? s : s.substr(sep + 1);
            if (base == "ffmpeg.exe") ++n;
        }
        CloseHandle(h);
    }
    return n;
}

static DWORD handle_count() {
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
}

static Spectral::GenerateConfig job_cfg(const std::string& in, const std::string& out) {
    Spectral::GenerateConfig cfg;
    cfg.input_path = in;
    cfg.output_path = out;
    cfg.fft_size = 1024;
    cfg.width = 256;
    cfg.height = 128;
    return cfg;
}

// --- taxonomy ---------------------------------------------------------------
static void test_taxonomy() {
    std::printf("[taxonomy]\n");
    CHECK(std::string(Spectral::job_error_name(Spectral::JobError::Cancelled)) ==
              "cancelled",
          "cancelled has a name");
    CHECK(static_cast<int>(Spectral::JobError::Cancelled) == 10, "cancelled appended as 10");
    CHECK(static_cast<int>(Spectral::JobError::EncodeError) == 9, "existing codes unmoved");
    CHECK(static_cast<int>(Spectral::JobError::Ok) == 0, "Ok still 0");
}

// --- 1. cancellation during decode (decoder observed mid-stream) -------------
static void test_decode_cancel() {
    std::printf("[decode_cancel]\n");
    const fs::path wav = g_dir / "long_decode.wav";
    write_wav(wav, 600);  // ~26MB: seconds of decode, cancel lands mid-stream
    const int base_ff = count_ffmpeg();
    const DWORD base_handles = handle_count();

    std::atomic<bool> flag{false};
    int64_t delivered = 0;
    int64_t total_est = 0;
    bool was_cancelled = false;
    bool was_failed = false;
    std::atomic<bool> done{false};
    // Steady-state handle check: the first cycle may pay one-time process
    // init costs; a real leak grows on EVERY cycle. Both cycles must
    // cancel mid-stream; handles must not grow between them.
    DWORD cycle_handles[2] = {0, 0};
    for (int cycle = 0; cycle < 2; ++cycle) {
        flag.store(false);
        delivered = 0;
        done.store(false);
        MediaDecoder dec;
        dec.set_cancel(&flag);
        CHECK(dec.open(wav.string()), "decoder opens long file");
        if (cycle == 0) {
            total_est = dec.total_frames();
            CHECK(total_est > 10000000, "long decode has tens of millions of frames");
        }
        std::thread worker([&] {
            AudioFrame fr;
            while (dec.read_frame(fr)) delivered += fr.samples.size();
            done = true;
        });
        // Wait until provably well into the stream, then request
        // immediately (no sleep: the remaining 3/4 still takes seconds).
        CHECK(wait_until([&] { return delivered > total_est / 4; }, 60000),
              "decode advanced mid-stream");
        flag.store(true);
        CHECK(wait_until([&] { return done.load(); }, 30000), "decode worker stopped");
        worker.join();
        if (cycle == 0) {
            was_cancelled = dec.cancelled();
            was_failed = dec.failed();
        } else {
            CHECK(dec.cancelled(), "second cycle also cancels mid-stream");
        }
        dec.close();
        // No ffmpeg child may survive cancellation.
        CHECK(wait_until([&] { return count_ffmpeg() == base_ff; }, 10000),
              "ffmpeg child terminated");
        cycle_handles[cycle] = handle_count();
    }
    CHECK(was_cancelled, "decoder reports cancelled");
    CHECK(!was_failed, "cancel is not a decode failure");
    CHECK(delivered > 0 && delivered < total_est, "stopped mid-stream, not at EOF");
    std::printf("  (handles: base=%lu c1=%lu c2=%lu)\n", base_handles, cycle_handles[0],
                cycle_handles[1]);
    CHECK(cycle_handles[1] == cycle_handles[0], "no handle growth per cancel cycle");
}

// --- 2. cancellation during analysis (progress-driven request) ---------------
static void test_analysis_cancel() {
    std::printf("[analysis_cancel]\n");
    const fs::path wav = g_dir / "long_analysis.wav";
    write_wav(wav, 120);
    const fs::path out = g_dir / "acancel.png";

    std::atomic<bool> flag{false};
    std::mutex mtx;
    std::string seen_stage;
    float seen_frac = 0.0f;
    Spectral::Error result = Spectral::Error::success();
    std::thread worker([&] {
        auto cfg = job_cfg(wav.string(), out.string());
        cfg.reassigned = true;  // heavy per-frame work: seconds of analysis
        cfg.fft_size = 2048;
        result = Spectral::run_job(
            cfg,
            [&](float f, const char* stage) {
                std::lock_guard<std::mutex> lk(mtx);
                seen_stage = stage;
                seen_frac = f;
                // Request cancellation while analysis is provably running.
                if (seen_stage == "analyze" && seen_frac > 0.03) flag.store(true);
            },
            &flag);
    });
    // The flag can only be set from inside the analysis progress callback.
    CHECK(wait_until([&] { return flag.load(); }, 60000), "cancel requested mid-analysis");
    worker.join();
    {
        std::lock_guard<std::mutex> lk(mtx);
        CHECK(seen_stage == "analyze", "request fired during analysis");
    }
    CHECK(result == Spectral::JobError::Cancelled, "analysis cancel -> Cancelled");
    CHECK(!fs::exists(out), "no partial output after analysis cancel");
    CHECK(!has_temp_residue(g_dir), "temp removed after analysis cancel");
}

// --- 3. cancellation before render -------------------------------------------
static void test_before_render_cancel() {
    std::printf("[before_render_cancel]\n");
    const fs::path wav = g_dir / "short.wav";
    write_wav(wav, 2);
    const fs::path out = g_dir / "rcancel.png";

    std::atomic<bool> flag{false};
    Spectral::Error result = Spectral::Error::success();
    std::thread worker([&] {
        result = Spectral::run_job(
            job_cfg(wav.string(), out.string()),
            [&](float f, const char* stage) {
                if (std::string(stage) == "render") flag.store(true);
                (void)f;
            },
            &flag);
    });
    worker.join();
    CHECK(result == Spectral::JobError::Cancelled, "pre-render cancel -> Cancelled");
    CHECK(!fs::exists(out), "no output after pre-render cancel");
    CHECK(!has_temp_residue(g_dir), "temp removed after pre-render cancel");
}

// --- 4. cancellation during video (frames + encoder abort) --------------------
static void test_video_cancel() {
    std::printf("[video_cancel]\n");
    if (!Spectral::VideoEncoder::ffmpeg_available()) {
        std::printf("  (ffmpeg missing: video cancel test skipped)\n");
        return;
    }
    const fs::path wav = g_dir / "long_video.wav";
    write_wav(wav, 120);  // 1800 frames at fps 15: many seconds of encoding
    const fs::path out = g_dir / "vcancel.mp4";
    const int base_ff = count_ffmpeg();

    std::atomic<bool> flag{false};
    Spectral::Error result = Spectral::Error::success();
    std::thread worker([&] {
        auto cfg = job_cfg(wav.string(), out.string());
        cfg.output_format = "video";
        cfg.output_format_explicit = true;
        cfg.width = 128;
        cfg.height = 64;
        cfg.fps = 15;
        result = Spectral::run_job(cfg, {}, &flag);
    });
    // Encoding runs for seconds (ffmpeg startup alone exceeds this delay).
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    flag.store(true);
    worker.join();
    CHECK(result == Spectral::JobError::Cancelled, "video cancel -> Cancelled");
    CHECK(!fs::exists(out), "no partial video after cancel");
    CHECK(!has_temp_residue(g_dir), "temp removed after video cancel");
    CHECK(wait_until([&] { return count_ffmpeg() == base_ff; }, 10000),
          "video ffmpeg child terminated");
}

// --- 5. kill during a blocked pipe read ---------------------------------------
static void test_kill_blocked_read() {
    std::printf("[kill_blocked_read]\n");
    Spectral::SafeProcess proc;
    Spectral::SafeProcess::Options opts;
    opts.capture_stdout = true;
    opts.capture_stderr = true;
    CHECK(proc.spawn(Spectral::resolve_tool("ffmpeg"),
                     {"-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=30",
                      "-f", "s16le", "-acodec", "pcm_s16le", "-"}, opts),
          "lavfi source spawned");
    std::vector<uint8_t> buf(65536);
    std::atomic<size_t> total{0};
    std::atomic<bool> reader_done{false};
    std::thread reader([&] {
        for (;;) {
            size_t n = proc.read_stdout(buf.data(), buf.size());
            if (n == 0 || n == static_cast<size_t>(-1)) break;
            total += n;
        }
        reader_done = true;
    });
    CHECK(wait_until([&] { return total.load() > 0; }, 30000), "stream flowing");
    proc.kill();  // from another thread while the reader is blocked in ReadFile
    CHECK(wait_until([&] { return reader_done.load(); }, 10000),
          "blocked reader released by kill");
    reader.join();
    CHECK(!proc.running(), "child dead after kill");
    CHECK(proc.stderr_text().size() < 1000000, "stderr drain joined, text bounded");
    proc.cleanup();
}

// --- 6. cancellation preserves a previous valid output -------------------------
static void test_cancel_preserves_output() {
    std::printf("[cancel_preserves_output]\n");
    const fs::path wav = g_dir / "tone.wav";
    write_wav(wav, 2);
    const fs::path out = g_dir / "keep.png";
    CHECK(Spectral::run_job(job_cfg(wav.string(), out.string())) == Spectral::JobError::Ok,
          "baseline output ok");
    std::vector<uint8_t> before;
    CHECK(read_bytes(out, before), "baseline readable");

    const fs::path long_wav = g_dir / "long_keep.wav";
    write_wav(long_wav, 120);
    std::atomic<bool> flag{false};
    Spectral::Error result = Spectral::Error::success();
    std::thread worker([&] {
        auto cfg = job_cfg(long_wav.string(), out.string());
        cfg.reassigned = true;
        cfg.fft_size = 2048;
        result = Spectral::run_job(
            cfg,
            [&](float f, const char* stage) {
                if (std::string(stage) == "analyze" && f > 0.03) flag.store(true);
            },
            &flag);
    });
    CHECK(wait_until([&] { return flag.load(); }, 60000), "cancel requested");
    worker.join();
    CHECK(result == Spectral::JobError::Cancelled, "cancel -> Cancelled");
    std::vector<uint8_t> after;
    CHECK(read_bytes(out, after) && after == before, "old output byte-identical");
    CHECK(!has_temp_residue(g_dir), "no temp residue");
}

// --- 7. batch cancellation ------------------------------------------------------
static void test_batch_cancel() {
    std::printf("[batch_cancel]\n");
    const fs::path in = g_dir / "cbin";
    const fs::path out = g_dir / "cbout";
    fs::create_directories(in);
    for (int i = 0; i < 3; ++i) write_wav(in / ("f" + std::to_string(i) + ".wav"), 30);
    Spectral::GenerateConfig tpl;
    tpl.width = 128;
    tpl.height = 64;
    tpl.fft_size = 512;
    Spectral::BatchOptions opts;
    opts.jobs = 1;  // sequential: file0 completes, rest must not succeed
    auto files = Spectral::collect_inputs(in.string(), opts);
    CHECK(files.size() == 3, "three batch inputs");

    std::atomic<bool> flag{false};
    std::vector<Spectral::FileResult> results;
    std::thread worker([&] {
        results = Spectral::run_batch(tpl, files, in.string(), out.string(), opts, flag, {});
    });
    // First completion proves in-flight work; cancel the remainder.
    CHECK(wait_until(
              [&] {
                  std::error_code ec;
                  for (auto it = fs::directory_iterator(out, ec);
                       it != fs::directory_iterator(); it.increment(ec)) {
                      if (ec) break;
                      if (it->path().extension() == ".png") return true;
                  }
                  return false;
              },
              120000),
          "first batch file completed");
    flag.store(true);
    worker.join();
    CHECK(results.size() == 3, "three results");
    CHECK(results[0].ok, "completed file stays valid");
    CHECK(fs::exists(results[0].output), "completed output exists");
    int ok_count = 0;
    for (auto& r : results) {
        if (r.ok) {
            ++ok_count;
            CHECK(fs::exists(r.output), "ok file has output");
        } else {
            CHECK(!fs::exists(r.output), "cancelled file has no partial output");
        }
    }
    CHECK(ok_count < 3, "not everything ran after cancel");
    CHECK(!has_temp_residue(out), "no temp residue in batch out");
}

// --- 8. CLI Ctrl-Break cancellation (real console event, real process) -----------
static void test_cli_cancel() {
    std::printf("[cli_cancel]\n");
    if (g_spectragen.empty() || !fs::exists(g_spectragen)) {
        std::printf("  (no spectragen binary: CLI cancel test skipped)\n");
        return;
    }
    const fs::path wav = g_dir / "cli_long.wav";
    write_wav(wav, 600);  // ~52MB + heavy analysis: tens of seconds of work
                          // even with the planned FFT engine
    const fs::path out = g_dir / "cli_cancel.png";
    const int base_ff = count_ffmpeg();

    // Own process group so the break event reaches the child, not us.
    std::string cmd = "\"" + g_spectragen + "\" \"" + wav.string() + "\" --output \"" +
                      out.string() + "\" --fft 8192 --reassigned --resolution 1024x512";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd_mut = cmd;
    CHECK(CreateProcessA(nullptr, cmd_mut.data(), nullptr, nullptr, FALSE,
                         CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi),
          "spectragen child spawned");
    CloseHandle(pi.hThread);
    // Decode plus heavy analysis run for tens of seconds; a short delay
    // lands the break provably mid-flight (mid-analysis, post-decode).
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi.dwProcessId), "break delivered");
    const DWORD wait = WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CHECK(wait == WAIT_OBJECT_0, "child exited after break");
    CHECK(code == 7, "CLI cancel exits 7");
    CHECK(!fs::exists(out), "no partial CLI output");
    CHECK(!has_temp_residue(g_dir), "no temp residue after CLI cancel");
    CHECK(wait_until([&] { return count_ffmpeg() == base_ff; }, 10000),
          "no ffmpeg survives CLI cancel");
}

int main(int argc, char** argv) {
    if (argc > 1) g_spectragen = argv[1];
    std::error_code ec;
    g_dir = fs::temp_directory_path(ec) / "svg_phase5_test";
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    if (ec) {
        std::printf("FAIL: cannot create test dir\n");
        return 1;
    }
    test_taxonomy();
    test_decode_cancel();
    test_analysis_cancel();
    test_before_render_cancel();
    test_video_cancel();
    test_kill_blocked_read();
    test_cancel_preserves_output();
    test_batch_cancel();
    test_cli_cancel();
    fs::remove_all(g_dir, ec);
    std::printf("\n=== cancellation: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
